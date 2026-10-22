#!/usr/bin/env python3
"""Build third-party libraries for BaseOS ELF programs into the toolchain sysroot.

    python3 ports/build.py            # zlib, libpng, libogg, opus, opusfile, iir1, SDL2, DOSBox

Needs build/toolchain (toolchain/build.py). Each library is pinned by version
and SHA-256 (or git commit), built as a static library with i686-baseos-gcc,
and installed with a pkg-config file into build/toolchain/sysroot/usr, where
the cross compiler finds it without extra flags:

    i686-baseos-gcc game.c $(pkg-config --static --libs sdl2)

with PKG_CONFIG_SYSROOT_DIR and PKG_CONFIG_LIBDIR set as env() below does.
SDL 2 gets the BaseOS video and audio drivers from ports/sdl2. Steps are
skipped when their stamp exists; --rebuild NAME forces one again.

The last step builds DOSBox Staging with ports/dosbox/dosbox-staging.patch
into build/ports/dosbox: the DOSBOX program, its resources folder and the
BaseOS settings file. Copy that folder anywhere BaseOS can read and start
DOSBOX; the settings file next to it keeps DOSBox in its portable layout.
"""
import argparse
import hashlib
import os
import pathlib
import shutil
import subprocess
import sys
import urllib.request
import zipfile

ROOT = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'toolchain'))
from build import TARGET, cross_file, fetch_tarball, log, run  # noqa: E402

HERE = ROOT / 'ports'
TOOLCHAIN = ROOT / 'build/toolchain'
SYSROOT = TOOLCHAIN / 'sysroot'
USR = SYSROOT / 'usr'
CFLAGS = ['-O2', '-g0', '-fno-strict-aliasing']

SOURCES = {
    'zlib': ('zlib-1.3.1', 'https://github.com/madler/zlib/releases/download/v1.3.1/zlib-1.3.1.tar.gz',
             '9a93b2b7dfdac77ceba5a558a580e74667dd6fede4585b91eefb60f03b72df23'),
    'libpng': ('libpng-1.6.44', 'https://download.sourceforge.net/libpng/libpng-1.6.44.tar.xz',
               '60c4da1d5b7f0aa8d158da48e8f8afa9773c1c8baa5d21974df61f1886b8ce8e'),
    'libogg': ('libogg-1.3.5', 'https://github.com/xiph/ogg/releases/download/v1.3.5/libogg-1.3.5.tar.xz',
               'c4d91be36fc8e54deae7575241e03f4211eb102afb3fc0775fbbc1b740016705'),
    'opus': ('opus-1.5.2', 'https://github.com/xiph/opus/releases/download/v1.5.2/opus-1.5.2.tar.gz',
             '65c1d2f78b9f2fb20082c38cbe47c951ad5839345876e46941612ee87f9a7ce1'),
    'opusfile': ('opusfile-0.12', 'https://github.com/xiph/opusfile/releases/download/v0.12/opusfile-0.12.tar.gz',
                 '118d8601c12dd6a44f52423e68ca9083cc9f2bfe72da7a8c1acb22a80ae3550b'),
    'sdl2': ('SDL2-2.30.9', 'https://github.com/libsdl-org/SDL/releases/download/release-2.30.9/SDL2-2.30.9.tar.gz',
             '24b574f71c87a763f50704bbb630cbe38298d544a1f890f099a4696b1d6beba4'),
}
IIR = ('iir1-1.9.3', 'https://github.com/berndporr/iir1.git', '1.9.3')
DOSBOX = ('dosbox-staging-0.82.2', 'https://github.com/dosbox-staging/dosbox-staging.git', 'v0.82.2',
          'f8c24f87d7fb0f41c2b7ef2a1aa97d16a52206be')
# DOSBox builds SpeexDSP from its meson wrap. xiph.org is not always
# reachable, so take the same release tag from GitHub and the wrap's build
# files from WrapDB, both pinned.
SPEEXDSP = ('speexdsp-1.2.1', 'https://github.com/xiph/speexdsp.git', 'SpeexDSP-1.2.1',
            '1b28a0f61bc31162979e1f26f3981fc3637095c8')
SPEEXDSP_WRAP = ('https://wrapdb.mesonbuild.com/v2/speexdsp_1.2.1-7/get_patch', 'speexdsp_1.2.1-7_patch.zip',
                 '2e6ea56de3e8c20089c038acc1eb63745004e3935929f40e0afcfcfe05532952')
OUTPUT = ROOT / 'build/ports'


def tool(name):
    return str(TOOLCHAIN / 'bin' / f'{TARGET}-{name}')


def env():
    """Environment in which pkg-config answers for the BaseOS sysroot."""
    result = dict(os.environ)
    result['PKG_CONFIG_SYSROOT_DIR'] = str(SYSROOT)
    result['PKG_CONFIG_LIBDIR'] = str(USR / 'lib/pkgconfig')
    result.pop('PKG_CONFIG_PATH', None)
    return result


def source(work, cache, name):
    directory, url, digest = SOURCES[name]
    tree = work / directory
    if tree.exists():
        shutil.rmtree(tree)
    archive = fetch_tarball(cache, directory, url, digest)
    run(['tar', 'xf', archive, '-C', work], cwd=work)
    return tree


def compile_library(tree, name, files, flags=(), cxx=False):
    """Compile files (relative to tree) and archive them as lib<name>.a."""
    objects = tree / '.baseos-objects'
    objects.mkdir(exist_ok=True)
    built = []
    compiler = tool('g++' if cxx else 'gcc')
    jobs = []
    for index, path in enumerate(files):
        output = objects / f'{index:04d}-{pathlib.Path(path).stem}.o'
        jobs.append(subprocess.Popen([compiler, *CFLAGS, *flags, '-c', str(tree / path), '-o', str(output)],
                                     cwd=tree))
        built.append(output)
        if len(jobs) >= (os.cpu_count() or 2):
            for job in jobs:
                if job.wait():
                    raise SystemExit(f'{name}: compiling failed')
            jobs = []
    for job in jobs:
        if job.wait():
            raise SystemExit(f'{name}: compiling failed')
    library = USR / 'lib' / f'lib{name}.a'
    library.unlink(missing_ok=True)
    log(f'archiving {library.name} ({len(built)} objects)')
    subprocess.run([tool('ar'), 'rcs', str(library), *map(str, built)], cwd=tree, check=True)


def install_headers(paths, destination):
    destination.mkdir(parents=True, exist_ok=True)
    for path in paths:
        shutil.copy(path, destination / pathlib.Path(path).name)


def pkgconfig(name, version, cflags, libs, requires=''):
    directory = USR / 'lib/pkgconfig'
    directory.mkdir(parents=True, exist_ok=True)
    (directory / f'{name}.pc').write_text(f"""prefix=/usr
includedir=${{prefix}}/include
libdir=${{prefix}}/lib

Name: {name}
Description: {name} for BaseOS
Version: {version}
Requires: {requires}
Cflags: {cflags}
Libs: {libs}
""")


def build_zlib(work, cache):
    tree = source(work, cache, 'zlib')
    files = ['adler32.c', 'compress.c', 'crc32.c', 'deflate.c', 'gzclose.c', 'gzlib.c', 'gzread.c',
             'gzwrite.c', 'infback.c', 'inffast.c', 'inflate.c', 'inftrees.c', 'trees.c', 'uncompr.c', 'zutil.c']
    compile_library(tree, 'z', files, ['-DHAVE_UNISTD_H', '-DHAVE_STDARG_H'])
    install_headers([tree / 'zlib.h', tree / 'zconf.h'], USR / 'include')
    pkgconfig('zlib', '1.3.1', '-I${includedir}', '-L${libdir} -lz')


def build_libpng(work, cache):
    tree = source(work, cache, 'libpng')
    shutil.copy(tree / 'scripts/pnglibconf.h.prebuilt', tree / 'pnglibconf.h')
    files = ['png.c', 'pngerror.c', 'pngget.c', 'pngmem.c', 'pngpread.c', 'pngread.c', 'pngrio.c',
             'pngrtran.c', 'pngrutil.c', 'pngset.c', 'pngtrans.c', 'pngwio.c', 'pngwrite.c', 'pngwtran.c',
             'pngwutil.c']
    compile_library(tree, 'png16', files, ['-DPNG_INTEL_SSE_OPT=0', '-I.'])
    shutil.copy(USR / 'lib/libpng16.a', USR / 'lib/libpng.a')
    install_headers([tree / 'png.h', tree / 'pngconf.h', tree / 'pnglibconf.h'], USR / 'include')
    pkgconfig('libpng', '1.6.44', '-I${includedir}', '-L${libdir} -lpng16', 'zlib')
    shutil.copy(USR / 'lib/pkgconfig/libpng.pc', USR / 'lib/pkgconfig/libpng16.pc')


def build_libogg(work, cache):
    tree = source(work, cache, 'libogg')
    (tree / 'include/ogg/config_types.h').write_text("""#ifndef __CONFIG_TYPES_H__
#define __CONFIG_TYPES_H__
#include <stdint.h>
typedef int16_t ogg_int16_t;
typedef uint16_t ogg_uint16_t;
typedef int32_t ogg_int32_t;
typedef uint32_t ogg_uint32_t;
typedef int64_t ogg_int64_t;
typedef uint64_t ogg_uint64_t;
#endif
""")
    compile_library(tree, 'ogg', ['src/bitwise.c', 'src/framing.c'], ['-Iinclude'])
    install_headers(sorted((tree / 'include/ogg').glob('*.h')), USR / 'include/ogg')
    pkgconfig('ogg', '1.3.5', '-I${includedir}', '-L${libdir} -logg')


def build_opus(work, cache):
    tree = source(work, cache, 'opus')
    build = tree / 'build-baseos'
    cross = work / 'baseos.cross'
    cross_file(TOOLCHAIN, cross)
    run(['meson', 'setup', build, '--cross-file', cross, '--prefix', '/usr', '--libdir', 'lib',
         '--default-library=static', '--buildtype=release', '-Dtests=disabled', '-Ddocs=disabled',
         '-Dextra-programs=disabled', '-Dintrinsics=disabled', '-Drtcd=disabled', '-Dasm=disabled'], cwd=tree)
    run(['ninja', '-C', build], cwd=tree)
    run(['meson', 'install', '-C', build, '--destdir', SYSROOT], cwd=tree)


def build_opusfile(work, cache):
    tree = source(work, cache, 'opusfile')
    files = ['src/info.c', 'src/internal.c', 'src/opusfile.c', 'src/stream.c']
    compile_library(tree, 'opusfile', files, ['-Iinclude', f'-I{USR}/include/opus', '-DOP_DISABLE_HTTP',
                                              '-DOP_HAVE_LRINTF'])
    install_headers([tree / 'include/opusfile.h'], USR / 'include/opus')
    pkgconfig('opusfile', '0.12', '-I${includedir}/opus', '-L${libdir} -lopusfile', 'ogg opus')


def git_tag(tree, url, tag, commit=None):
    """A fresh checkout of tag in tree; commit, when given, must match."""
    if tree.exists():
        shutil.rmtree(tree)
    tree.parent.mkdir(parents=True, exist_ok=True)
    run(['git', 'init', '-q', tree], cwd=tree.parent)
    run(['git', 'fetch', '-q', '--depth', '1', url, f'refs/tags/{tag}'], cwd=tree)
    run(['git', 'checkout', '-q', 'FETCH_HEAD'], cwd=tree)
    if commit:
        head = subprocess.run(['git', 'rev-parse', 'HEAD^{commit}'], cwd=tree, check=True,
                              capture_output=True, text=True).stdout.strip()
        if head != commit:
            raise SystemExit(f'{tree.name}: tag {tag} is {head}, expected {commit}')


def build_iir(work, cache):
    directory, url, version = IIR
    tree = work / directory
    git_tag(tree, url, version)
    files = sorted(str(path.relative_to(tree)) for path in (tree / 'iir').glob('*.cpp'))
    compile_library(tree, 'iir', files, ['-std=c++17', '-I.'], cxx=True)
    install_headers([tree / 'Iir.h'], USR / 'include')
    install_headers(sorted((tree / 'iir').glob('*.h')), USR / 'include/iir')
    pkgconfig('iir', version, '-I${includedir}', '-L${libdir} -liir')


SDL_DIRECTORIES = ['', 'atomic', 'audio', 'audio/dummy', 'audio/baseos', 'cpuinfo', 'dynapi', 'events',
                   'file', 'filesystem/unix', 'haptic', 'haptic/dummy', 'hidapi', 'joystick',
                   'joystick/dummy', 'joystick/virtual', 'libm', 'loadso/dummy', 'locale', 'locale/dummy',
                   'misc', 'misc/dummy', 'power', 'render', 'render/software', 'sensor', 'sensor/dummy',
                   'stdlib', 'thread', 'thread/pthread', 'timer', 'timer/unix', 'video', 'video/dummy',
                   'video/baseos', 'video/yuv2rgb']


def build_sdl2(work, cache):
    tree = source(work, cache, 'sdl2')
    (tree / 'src/video/baseos').mkdir()
    (tree / 'src/audio/baseos').mkdir()
    shutil.copy(HERE / 'sdl2/SDL_baseosvideo.c', tree / 'src/video/baseos/SDL_baseosvideo.c')
    shutil.copy(HERE / 'sdl2/SDL_baseosaudio.c', tree / 'src/audio/baseos/SDL_baseosaudio.c')
    shutil.copy(HERE / 'sdl2/SDL_config.h', tree / 'include/SDL_config.h')
    # Register the drivers ahead of the dummy ones.
    edits = {
        'src/video/SDL_sysvideo.h': ('extern VideoBootStrap DUMMY_bootstrap;',
                                     'extern VideoBootStrap BASEOS_bootstrap;\nextern VideoBootStrap DUMMY_bootstrap;'),
        'src/video/SDL_video.c': ('    &DUMMY_bootstrap,',
                                  '#ifdef SDL_VIDEO_DRIVER_BASEOS\n    &BASEOS_bootstrap,\n#endif\n    &DUMMY_bootstrap,'),
        'src/audio/SDL_sysaudio.h': ('extern AudioBootStrap DUMMYAUDIO_bootstrap;',
                                     'extern AudioBootStrap BASEOS_audio_bootstrap;\n'
                                     'extern AudioBootStrap DUMMYAUDIO_bootstrap;'),
        'src/audio/SDL_audio.c': ('    &DUMMYAUDIO_bootstrap,',
                                  '#ifdef SDL_AUDIO_DRIVER_BASEOS\n    &BASEOS_audio_bootstrap,\n#endif\n'
                                  '    &DUMMYAUDIO_bootstrap,'),
    }
    for path, (old, new) in edits.items():
        text = (tree / path).read_text()
        if text.count(old) != 1:
            raise SystemExit(f'SDL2: cannot patch {path}')
        (tree / path).write_text(text.replace(old, new))
    files = []
    for directory in SDL_DIRECTORIES:
        files += sorted(str(path.relative_to(tree)) for path in (tree / 'src' / directory).glob('*.c'))
    compile_library(tree, 'SDL2', files, ['-Iinclude', '-Isrc', '-D_REENTRANT', '-Wno-deprecated-declarations'])
    headers = USR / 'include/SDL2'
    if headers.exists():
        shutil.rmtree(headers)
    install_headers(sorted((tree / 'include').glob('*.h')), headers)
    for unused in headers.glob('SDL_config_*.h'):
        unused.unlink()
    pkgconfig('sdl2', '2.30.9', '-I${includedir}/SDL2 -D_REENTRANT', '-L${libdir} -lSDL2 -lpthread')


def dosbox_cross_file(path):
    binary = TOOLCHAIN / 'bin'
    path.write_text(f"""[binaries]
c = '{binary}/{TARGET}-gcc'
cpp = '{binary}/{TARGET}-g++'
ar = '{binary}/{TARGET}-ar'
strip = '{binary}/{TARGET}-strip'
pkg-config = 'pkg-config'

[properties]
sys_root = '{SYSROOT}'
pkg_config_libdir = '{USR}/lib/pkgconfig'

[host_machine]
system = 'baseos'
cpu_family = 'x86'
cpu = 'i686'
endian = 'little'
""")


def build_dosbox(work, cache):
    directory, url, tag, commit = DOSBOX
    tree = work / directory
    git_tag(tree, url, tag, commit)
    run(['git', 'apply', HERE / 'dosbox/dosbox-staging.patch'], cwd=tree)
    name, speex_url, speex_tag, speex_commit = SPEEXDSP
    speex = tree / 'subprojects' / name
    git_tag(speex, speex_url, speex_tag, speex_commit)
    wrap_url, wrap_name, wrap_digest = SPEEXDSP_WRAP
    archive = cache / wrap_name
    if not archive.exists():
        log(f'downloading {wrap_url}')
        with urllib.request.urlopen(wrap_url) as response:
            archive.write_bytes(response.read())
    if hashlib.sha256(archive.read_bytes()).hexdigest() != wrap_digest:
        raise SystemExit(f'checksum mismatch for {archive}')
    with zipfile.ZipFile(archive) as bundle:
        bundle.extractall(tree / 'subprojects')
    build = tree / 'build-baseos'
    cross = work / 'dosbox.cross'
    dosbox_cross_file(cross)
    # Programs cannot run at configure time, so give the page size and turn
    # off the parts that need OpenGL, MIDI synthesizers or networking.
    run(['meson', 'setup', build, '--cross-file', cross, '--buildtype', 'release',
         '-Ddefault_library=static', '-Dunit_tests=disabled', '-Dtry_static_libs=[]',
         '-Duse_opengl=false', '-Duse_fluidsynth=false', '-Duse_mt32emu=false', '-Duse_slirp=false',
         '-Duse_sdl2_net=false', '-Duse_alsa=false', '-Duse_zlib_ng=false', '-Ddynamic_core=none',
         '-Dpagesize=4096'], cwd=tree, env=env())
    run(['ninja', '-C', build], cwd=tree, env=env())
    target = OUTPUT / 'dosbox'
    if target.exists():
        shutil.rmtree(target)
    target.mkdir(parents=True)
    run([tool('strip'), '-o', target / 'DOSBOX', build / 'dosbox'], cwd=tree)
    shutil.copytree(build / 'resources', target / 'resources', symlinks=False)
    shutil.copy(HERE / 'dosbox/dosbox-staging.conf', target / 'dosbox-staging.conf')
    log(f'DOSBox ready in {target}')


STEPS = [('zlib', build_zlib), ('libpng', build_libpng), ('libogg', build_libogg), ('opus', build_opus),
         ('opusfile', build_opusfile), ('iir', build_iir), ('sdl2', build_sdl2), ('dosbox', build_dosbox)]


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--cache', type=pathlib.Path, default=ROOT / 'build/ports-src')
    parser.add_argument('--rebuild', action='append', default=[], choices=[name for name, _ in STEPS])
    parser.add_argument('only', nargs='*', help='build just these libraries')
    args = parser.parse_args()
    if not (TOOLCHAIN / 'bin' / f'{TARGET}-gcc').exists():
        raise SystemExit('Build the toolchain first: python3 toolchain/build.py')
    work = args.cache / 'work'
    work.mkdir(parents=True, exist_ok=True)
    (USR / 'lib').mkdir(parents=True, exist_ok=True)
    # Programs and ports see the current system call header.
    shutil.copy(ROOT / 'sdk/baseos_sys.h', USR / 'include/baseos_sys.h')
    stamps = USR / 'lib/.ports-stamps'
    stamps.mkdir(exist_ok=True)
    for name, action in STEPS:
        if args.only and name not in args.only:
            continue
        marker = stamps / name
        if marker.exists() and name not in args.rebuild:
            log(f'{name}: up to date')
            continue
        log(f'building {name}')
        action(work, args.cache)
        marker.write_text('')
    log('ports installed in ' + str(USR))


if __name__ == '__main__':
    main()
