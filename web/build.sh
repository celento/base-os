#!/usr/bin/env bash
# Builds BaseOS from source and stages a static site in web/dist.
# Vercel runs this on every deploy (see vercel.json), so the site always
# serves disk images built from the deployed commit. Locally:
#   npm --prefix web ci && bash web/build.sh && npm --prefix web run serve
set -euo pipefail

WEB="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(dirname "$WEB")"
DIST="$WEB/dist"
OUT="$WEB/.build"

if [ ! -d "$WEB/node_modules/v86" ]; then
    (cd "$WEB" && npm ci --no-audit --no-fund)
fi

# Vercel's build image is Amazon Linux 2023. Install any missing C toolchain
# pieces with dnf; NASM falls back to the pinned npm binary when absent.
need=()
command -v gcc >/dev/null || need+=(gcc)
command -v ld >/dev/null || need+=(binutils)
command -v objcopy >/dev/null || need+=(binutils)
command -v make >/dev/null || need+=(make)
command -v python3 >/dev/null || need+=(python3)
if ((${#need[@]})); then
    if command -v dnf >/dev/null; then dnf install -y "${need[@]}"
    elif command -v apt-get >/dev/null; then apt-get update && apt-get install -y "${need[@]}"
    else echo "Missing build tools: ${need[*]}" >&2; exit 1
    fi
fi
NASM="$(command -v nasm || true)"
if [ -z "$NASM" ]; then
    NASM="$WEB/node_modules/nasm-runtime-linux-x64/bin/nasm"
    [ -x "$NASM" ] || { echo "NASM not found and npm fallback missing" >&2; exit 1; }
fi
echo "Using $NASM ($("$NASM" -v))"

# Build into a fresh directory every time so the site never ships a stale image.
rm -rf "$OUT" "$DIST"
make -C "$ROOT" -j"$(nproc 2>/dev/null || echo 2)" OUT="$OUT" AS="$NASM" \
    "$OUT/baseos.img" "$OUT/baseos-data.img"

V86="$WEB/node_modules/v86/build"
mkdir -p "$DIST/v86" "$DIST/bios" "$DIST/images"
cp "$WEB/index.html" "$WEB/app.js" "$WEB/style.css" "$DIST/"
cp "$V86/libv86.js" "$V86/v86.wasm" "$DIST/v86/"
cp "$WEB/bios/seabios.bin" "$WEB/bios/vgabios.bin" "$DIST/bios/"
gzip -9nc "$OUT/baseos.img" > "$DIST/images/baseos.img.gz"
gzip -9nc "$OUT/baseos-data.img" > "$DIST/images/baseos-data.img.gz"

python3 - "$DIST" "$OUT" "$ROOT" <<'EOF'
import hashlib, json, os, pathlib, subprocess, sys
dist, out, root = map(pathlib.Path, sys.argv[1:])
def revision():
    sha = os.environ.get("VERCEL_GIT_COMMIT_SHA")
    if sha:
        return sha[:12]
    try:
        return subprocess.check_output(["git", "rev-parse", "--short=12", "HEAD"],
                                       cwd=root, text=True, stderr=subprocess.DEVNULL).strip()
    except Exception:
        return "local"
images = {}
for name in ("baseos.img", "baseos-data.img"):
    data = (out / name).read_bytes()
    images[name] = {"size": len(data), "sha256": hashlib.sha256(data).hexdigest()[:16]}
manifest = {"revision": revision(), "images": images}
(dist / "build.json").write_text(json.dumps(manifest, indent=2) + "\n")
print(json.dumps(manifest))
EOF
echo "Staged $DIST"
