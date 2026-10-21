#!/usr/bin/env python3
"""Build the i686-baseos cross toolchain: binutils, GCC (C and C++) and mlibc.

The result is a normal GNU cross toolchain for static ELF32 BaseOS programs:

    build/toolchain/bin/i686-baseos-gcc hello.c -o hello

Steps run in dependency order and each one is skipped when its stamp file
exists, so an interrupted build resumes where it stopped. Sources are pinned by
version and SHA-256 (or git commit) and downloaded once into the source cache.

Host requirements: a C/C++ compiler, make, bison, flex, texinfo, GMP, MPFR,
MPC, meson (1.3 or newer), ninja, git and curl.
"""
import argparse
import hashlib
import os
import pathlib
import shutil
import subprocess
import sys
import urllib.request

ROOT = pathlib.Path(__file__).resolve().parents[1]
HERE = ROOT / 'toolchain'
TARGET = 'i686-baseos'

BINUTILS = ('binutils-2.43.1', 'https://ftp.gnu.org/gnu/binutils/binutils-2.43.1.tar.xz',
            '13f74202a3c4c51118b797a39ea4200d3f6cfbe224da6d1d95bb938480132dfd')
GCC = ('gcc-14.2.0', 'https://ftp.gnu.org/gnu/gcc/gcc-14.2.0/gcc-14.2.0.tar.xz',
       'a7b39bc69cbf9e25826c5a60ab26477001f7c08d85cec04bc0e29cabed6f3cc9')
MLIBC = ('mlibc', 'https://github.com/managarm/mlibc.git',
         'a1858073acd9c28b0b03bf43f8b2c8f872858c45')


def log(message):
    print(f'[toolchain] {message}', flush=True)


def run(command, cwd, env=None):
    log(' '.join(str(part) for part in command))
    subprocess.run([str(part) for part in command], cwd=cwd, env=env, check=True)


def sha256(path):
    digest = hashlib.sha256()
    with open(path, 'rb') as stream:
        for block in iter(lambda: stream.read(1 << 20), b''):
            digest.update(block)
    return digest.hexdigest()


def fetch_tarball(cache, name, url, digest):
    archive = cache / url.rsplit('/', 1)[1]
    if not archive.exists():
        log(f'downloading {url}')
        partial = archive.with_suffix('.partial')
        with urllib.request.urlopen(url) as response, open(partial, 'wb') as output:
            shutil.copyfileobj(response, output)
        partial.rename(archive)
    if sha256(archive) != digest:
        raise SystemExit(f'checksum mismatch for {archive}')
    return archive


def unpack(cache, work, name, url, digest, patch):
    source = work / name
    if (source / '.baseos-patched').exists():
        return source
    if source.exists():
        shutil.rmtree(source)
    archive = fetch_tarball(cache, name, url, digest)
    run(['tar', 'xf', archive, '-C', work], cwd=work)
    run(['patch', '-p1', '-i', patch], cwd=source)
    (source / '.baseos-patched').write_text('')
    return source


def checkout_mlibc(work):
    name, url, commit = MLIBC
    source = work / name
    if (source / '.baseos-patched').exists():
        return source
    if source.exists():
        shutil.rmtree(source)
    run(['git', 'init', '-q', source], cwd=work)
    run(['git', 'fetch', '-q', '--depth', '1', url, commit], cwd=source)
    run(['git', 'checkout', '-q', 'FETCH_HEAD'], cwd=source)
    run(['git', 'apply', HERE / 'patches/mlibc.patch'], cwd=source)
    sysdeps = source / 'sysdeps/baseos'
    shutil.copytree(HERE / 'mlibc-sysdeps', sysdeps)
    shutil.copy(ROOT / 'sdk/baseos_sys.h', sysdeps / 'include/baseos_sys.h')
    # abi-bits mirror Linux i386 numbering; link them like the upstream ports do.
    abi = sysdeps / 'include/abi-bits'
    abi.mkdir(parents=True, exist_ok=True)
    for header in sorted((source / 'abis/linux').glob('*.h')):
        link = abi / header.name
        if not link.exists():
            link.symlink_to(os.path.relpath(header, abi))
    (source / '.baseos-patched').write_text('')
    return source


def stamp(prefix, step):
    return prefix / '.stamps' / step


def step(prefix, name, action):
    marker = stamp(prefix, name)
    if marker.exists():
        log(f'{name}: up to date')
        return
    action()
    marker.parent.mkdir(parents=True, exist_ok=True)
    marker.write_text('')


def cross_file(prefix, path):
    binary = prefix / 'bin'
    path.write_text(f"""[binaries]
c = '{binary}/{TARGET}-gcc'
cpp = '{binary}/{TARGET}-g++'
ar = '{binary}/{TARGET}-ar'
strip = '{binary}/{TARGET}-strip'

[host_machine]
system = 'baseos'
cpu_family = 'x86'
cpu = 'i686'
endian = 'little'
""")


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--prefix', type=pathlib.Path, default=ROOT / 'build/toolchain')
    parser.add_argument('--cache', type=pathlib.Path, default=ROOT / 'build/toolchain-src')
    parser.add_argument('--jobs', type=int, default=os.cpu_count() or 2)
    parser.add_argument('--rebuild-mlibc', action='store_true',
                        help='rebuild and reinstall mlibc (after changing its sysdeps)')
    args = parser.parse_args()
    prefix, cache = args.prefix.resolve(), args.cache.resolve()
    sysroot = prefix / 'sysroot'
    work = cache / 'work'
    for directory in (prefix, sysroot / 'usr/include', sysroot / 'usr/lib', cache, work):
        directory.mkdir(parents=True, exist_ok=True)
    env = dict(os.environ, PATH=f'{prefix / "bin"}{os.pathsep}{os.environ["PATH"]}')
    jobs = f'-j{args.jobs}'

    binutils = unpack(cache, work, *BINUTILS, HERE / 'patches/binutils-2.43.1.patch')
    gcc = unpack(cache, work, *GCC, HERE / 'patches/gcc-14.2.0.patch')
    if args.rebuild_mlibc:
        for name in ('mlibc', 'mlibc-headers', 'libstdcxx'):
            stamp(prefix, name).unlink(missing_ok=True)
        shutil.rmtree(work / 'mlibc', ignore_errors=True)
        shutil.rmtree(work / 'build-mlibc', ignore_errors=True)
        shutil.rmtree(work / 'build-mlibc-headers', ignore_errors=True)
    mlibc = checkout_mlibc(work)
    meson_cross = work / 'baseos-cross.ini'
    cross_file(prefix, meson_cross)

    def build_binutils():
        build = work / 'build-binutils'
        shutil.rmtree(build, ignore_errors=True)
        build.mkdir()
        run([binutils / 'configure', f'--target={TARGET}', f'--prefix={prefix}',
             f'--with-sysroot={sysroot}', '--disable-nls', '--disable-werror',
             '--disable-gdb', '--disable-gprofng'], cwd=build, env=env)
        run(['make', jobs], cwd=build, env=env)
        run(['make', 'install'], cwd=build, env=env)

    def mlibc_headers():
        build = work / 'build-mlibc-headers'
        shutil.rmtree(build, ignore_errors=True)
        run(['meson', 'setup', build, mlibc, '--prefix=/usr', '-Dheaders_only=true',
             f'--cross-file={meson_cross}'], cwd=work, env=env)
        run(['ninja', '-C', build], cwd=work, env=env)
        run(['meson', 'install', '-C', build, f'--destdir={sysroot}', '--quiet'], cwd=work, env=env)

    gcc_build = work / 'build-gcc'

    def gcc_stage1():
        shutil.rmtree(gcc_build, ignore_errors=True)
        gcc_build.mkdir()
        run([gcc / 'configure', f'--target={TARGET}', f'--prefix={prefix}',
             f'--with-sysroot={sysroot}', '--enable-languages=c,c++', '--disable-nls',
             '--disable-multilib', '--disable-shared', '--enable-threads=posix',
             '--with-arch=i686', '--with-tune=generic', '--enable-initfini-array',
             '--disable-libssp', '--disable-libquadmath', '--disable-libgomp',
             '--disable-libatomic', '--disable-libitm', '--disable-libsanitizer',
             '--disable-libvtv', '--disable-bootstrap', '--enable-tls'],
            cwd=gcc_build, env=env)
        run(['make', jobs, 'all-gcc', 'all-target-libgcc'], cwd=gcc_build, env=env)
        run(['make', 'install-gcc', 'install-target-libgcc'], cwd=gcc_build, env=env)

    def build_mlibc():
        build = work / 'build-mlibc'
        shutil.rmtree(build, ignore_errors=True)
        run(['meson', 'setup', build, mlibc, '--prefix=/usr', '--buildtype=release',
             '-Ddefault_library=static', '-Duse_freestnd_hdrs=enabled',
             '-Dlinux_option=disabled', '-Dglibc_option=enabled', '-Dbsd_option=enabled',
             f'--cross-file={meson_cross}'], cwd=work, env=env)
        run(['ninja', '-C', build], cwd=work, env=env)
        run(['meson', 'install', '-C', build, f'--destdir={sysroot}', '--quiet'], cwd=work, env=env)

    def libstdcxx():
        run(['make', jobs, 'all-target-libstdc++-v3'], cwd=gcc_build, env=env)
        run(['make', 'install-target-libstdc++-v3'], cwd=gcc_build, env=env)

    step(prefix, 'binutils', build_binutils)
    step(prefix, 'mlibc-headers', mlibc_headers)
    step(prefix, 'gcc-stage1', gcc_stage1)
    step(prefix, 'mlibc', build_mlibc)
    step(prefix, 'libstdcxx', libstdcxx)
    log(f'ready: {prefix / "bin" / (TARGET + "-gcc")}')


if __name__ == '__main__':
    sys.exit(main())
