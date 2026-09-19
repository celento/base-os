"""Capture six real guest screenshots with sample data on a disposable image.

Usage: python3 tools/gallery.py build
The regular kernel and saved disk are untouched. A documentation-only guest
fixture opens the apps, supplies sample content, and waits for each QMP capture.
"""
import json
import pathlib
import shutil
import subprocess
import sys
import tempfile
import time
from layout import constants

ROOT = pathlib.Path(__file__).resolve().parents[1]
BUILD = pathlib.Path(sys.argv[1] if len(sys.argv) > 1 else "build").resolve()
OUTPUT = ROOT / "screenshots" / "gallery"
NAMES = ["desktop", "paint", "programming", "games", "themes", "search"]


def tool(name):
    return "x86_64-elf-" + name if shutil.which("x86_64-elf-" + name) else name


with tempfile.TemporaryDirectory(prefix="baseos-gallery-") as temp:
    work = pathlib.Path(temp)
    subprocess.run([tool("gcc"), "-Os", "-ffreestanding", "-m32", "-fno-pie",
                    "-fno-stack-protector", "-fno-builtin", "-mno-sse", "-mno-mmx",
                    "-msoft-float", "-I", str(ROOT), "-I", str(ROOT / "src"),
                    "-I", str(BUILD), "-c", str(ROOT / "tools/gallery_guest.c"),
                    "-o", str(work / "kernel.o")], check=True)
    objects = [str(p) for p in BUILD.glob("*.o") if p.name != "kernel.o"]
    subprocess.run([tool("ld"), "-T", str(BUILD / "linker.ld"), "-nostdlib", "-m",
                    "elf_i386", "-z", "noexecstack", "-o", str(work / "kernel.elf"),
                    str(work / "kernel.o"), *objects], check=True)
    subprocess.run([tool("objcopy"), "-O", "binary", str(work / "kernel.elf"),
                    str(work / "kernel.bin")], check=True)
    layout = constants()
    kernel = (work / "kernel.bin").read_bytes()
    assert len(kernel) <= layout["KERNEL_SECTORS"] * 512
    disk = bytearray(layout["DISK_SECTORS"] * 512)
    disk[:512] = (BUILD / "boot.bin").read_bytes()
    disk[512:512 + len(kernel)] = kernel
    (work / "disk.img").write_bytes(disk)
    log = work / "serial.log"
    OUTPUT.mkdir(parents=True, exist_ok=True)
    guest = subprocess.Popen([
        "qemu-system-i386", "-m", "32M", "-vga", "std", "-drive",
        f"file={work / 'disk.img'},format=raw,index=0,if=floppy", "-display", "none",
        "-serial", f"file:{log}", "-qmp", "stdio", "-no-reboot"],
        stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    try:
        if not guest.stdout.readline():
            raise RuntimeError(guest.stderr.read().decode())

        def qmp(command, arguments=None):
            guest.stdin.write(json.dumps({"execute": command, "arguments": arguments or {}}).encode() + b"\n")
            guest.stdin.flush()
            while True:
                line = guest.stdout.readline()
                if not line:
                    raise RuntimeError("QEMU closed its control channel")
                response = json.loads(line)
                if "error" in response:
                    raise RuntimeError(response)
                if "return" in response:
                    return response["return"]

        qmp("qmp_capabilities")
        for number, name in enumerate(NAMES):
            deadline = time.monotonic() + 45
            marker = f"GALLERY {number}\n"
            while not log.exists() or marker not in log.read_text():
                if guest.poll() is not None or time.monotonic() > deadline:
                    raise RuntimeError(log.read_text() if log.exists() else "Guest did not boot")
                time.sleep(0.1)
            time.sleep(0.5)  # Allow QEMU to refresh its display surface.
            target = OUTPUT / f"{name}.png"
            qmp("stop")
            qmp("screendump", {"filename": str(target), "format": "png"})
            qmp("cont")
            print(target, flush=True)
            qmp("human-monitor-command", {"command-line": "sendkey spc 35"})
            time.sleep(0.1)
    finally:
        guest.terminate()
        guest.wait(timeout=5)
