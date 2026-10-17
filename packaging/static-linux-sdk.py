#!/usr/bin/env python3
"""Dependency-only static Linux recipe; its exact bytes bind the SDK cache."""
import hashlib
import inspect
import json
import subprocess
import shutil


def run_stage(step, stamp, identity):
    if stamp.is_file() and stamp.read_text().strip() == identity:
        print('Reusing checked static SDK component: ' + stamp.name, flush=True)
        return
    step()
    stamp.parent.mkdir(parents=True, exist_ok=True)
    stamp.write_text(identity + '\n')


def build(args, base, prefix, sources, environment, jobs, ROOT, run, static_desktop, static_pipewire):
    def desktop():
        static_desktop.build(base, prefix, args.jobs, environment)
    def util_linux():
        # Ubuntu's libmount-dev contains no static archive. Build its matching
        # family of mount/blkid/uuid libraries without unrelated system programs.
        run('./configure', '--prefix=' + str(prefix), '--libdir=' + str(prefix / 'lib'),
            '--disable-all-programs', '--enable-libmount', '--enable-libblkid',
            '--enable-libuuid', '--enable-static', '--disable-shared', '--without-selinux',
            '--disable-nls', cwd=sources['util-linux'], env=environment)
        run('make', jobs, cwd=sources['util-linux'], env=environment)
        run('make', 'install', cwd=sources['util-linux'], env=environment)
    def pipewire():
        # PipeWire upstream explicitly uses shared_library(), ignoring default_library.
        # Build a static counterpart from exactly the same compiled client objects.
        pipewire = sources['pipewire']
        static_pipewire.prepare(pipewire)
        pwbuild = base / 'pipewire-build'
        if pwbuild.exists():
            shutil.rmtree(pwbuild)
        run('meson', 'setup', pwbuild, pipewire, '--prefix=/usr', '--libdir=lib/x86_64-linux-gnu',
            '--auto-features=disabled', '-Dexamples=disabled', '-Dtests=disabled',
            '-Dspa-plugins=enabled', '-Dsupport=enabled', '-Daudioconvert=enabled',
            '-Daudiomixer=enabled', '-Dvideoconvert=enabled', '-Dpipewire-jack=disabled', '-Dpipewire-v4l2=disabled', env=environment)
        run('ninja', '-C', pwbuild, jobs, env=environment)
        (prefix / 'lib').mkdir(exist_ok=True)
        static_pipewire.build(pwbuild, prefix, environment)
        # Install the same build's public/generated headers and pkg-config files.
        # Distro headers can describe an older ABI and must never drive this build.
        pwstage = base / 'pipewire-install'
        run('meson', 'install', '-C', pwbuild, '--destdir', pwstage, env=environment)
        shutil.copytree(pwstage / 'usr', prefix, dirs_exist_ok=True)
        for metadata in prefix.rglob('*.pc'):
            text = metadata.read_text().replace('prefix=/usr', 'prefix=' + str(prefix))
            metadata.write_text(text)
        shutil.copy2(prefix / 'lib/libpipewire-0.3.a', prefix / 'lib/x86_64-linux-gnu/libpipewire-0.3.a')
    def media():
        run('make', jobs, 'BUILDTYPE=Release', 'libraries', cwd=sources['openh264'], env=environment)
        run('make', 'PREFIX=' + str(prefix), 'install-static', cwd=sources['openh264'], env=environment)
        media = json.loads((ROOT / 'packaging/media-lgpl-lock.json').read_text())
        flags = [x for x in media['ffmpeg_flags'] if x not in ('--enable-shared', '--disable-static')]
        run('./configure', '--prefix=' + str(prefix), '--enable-static', '--disable-shared',
            '--pkg-config-flags=--static', *flags, cwd=sources['ffmpeg'], env=environment)
        run('make', jobs, cwd=sources['ffmpeg'], env=environment)
        run('make', 'install', cwd=sources['ffmpeg'], env=environment)
    def model_https():
        # Only Pro links this archive; the shared SDK also serves Community.
        run('python3', ROOT / 'packaging/static-model-https.py', '--prefix', prefix,
            '--source', sources['curl'], '--jobs', args.jobs, env=environment)
    def sdl():
        main_cmake = sources['SDL'] / 'CMakeLists.txt'
        content = main_cmake.read_text()
        original = '\n  CheckDLOPEN()\n'
        if content.count(original) != 1:
            raise ValueError('SDL load-object selection changed')
        main_cmake.write_text(content.replace(original,
            '\n  # Cast static delivery: use SDL load-object dummy backend.\n  set(HAVE_DLOPEN FALSE)\n'))
        # SDL's Wayland probe otherwise exports -lEGL even with every GL path off.
        checks = sources['SDL'] / 'cmake/sdlchecks.cmake'
        original = '    set(WAYLAND_PKG_CONFIG_SPEC "wayland-client>=1.18" wayland-egl wayland-cursor egl "xkbcommon>=0.5.0")'
        replacement = '    set(WAYLAND_PKG_CONFIG_SPEC "wayland-client>=1.18" wayland-egl wayland-cursor "xkbcommon>=0.5.0")\n    if(SDL_OPENGL OR SDL_OPENGLES)\n      list(APPEND WAYLAND_PKG_CONFIG_SPEC egl)\n    endif()'
        content = checks.read_text()
        if content.count(original) != 1:
            raise ValueError('SDL Wayland dependency recipe changed')
        checks.write_text(content.replace(original, replacement))
        dynapi = sources['SDL'] / 'src/dynapi/SDL_dynapi.h'
        content = dynapi.read_text()
        original = '#define SDL_DYNAMIC_API 1'
        if content.count(original) != 1:
            raise ValueError('SDL dynamic API master switch changed')
        dynapi.write_text(content.replace(original, '#define SDL_DYNAMIC_API 0 /* Cast static delivery: no replaceable shared SDL API. */'))
        for name, options in [
            ('SDL', ['-DSDL_SHARED=OFF', '-DSDL_STATIC=ON', '-DSDL_TESTS=OFF',
                     '-DSDL_TEST_LIBRARY=OFF', '-DSDL_X11=ON', '-DSDL_WAYLAND=OFF',
                     '-DSDL_DEPS_SHARED=OFF', '-DSDL_X11_SHARED=OFF', '-DSDL_WAYLAND_SHARED=OFF',
                     '-DSDL_WAYLAND_LIBDECOR=OFF', '-DSDL_OPENGL=OFF', '-DSDL_OPENGLES=OFF',
                     '-DSDL_VULKAN=OFF', '-DSDL_RENDER_GPU=OFF', '-DSDL_GPU=OFF', '-DSDL_AUDIO=OFF', '-DSDL_CAMERA=OFF',
                     '-DSDL_LIBUDEV=OFF', '-DSDL_JOYSTICK=OFF', '-DSDL_HAPTIC=OFF', '-DSDL_SENSOR=OFF',
                     '-DSDL_HIDAPI=OFF', '-DSDL_HIDAPI_LIBUSB=OFF', '-DSDL_DBUS=OFF', '-DSDL_IBUS=OFF']),
            ('SDL_ttf', ['-DBUILD_SHARED_LIBS=OFF', '-DSDLTTF_SAMPLES=OFF', '-DSDLTTF_HARFBUZZ=OFF',
                         '-DSDLTTF_PLUTOSVG=OFF'])]:
            build = base / (name + '-build')
            run('cmake', '-S', sources[name], '-B', build, '-G', 'Ninja',
                '-DCMAKE_BUILD_TYPE=Release', '-DCMAKE_INSTALL_PREFIX=' + str(prefix),
                '-DCMAKE_INSTALL_LIBDIR=lib', *options, env=environment)
            run('cmake', '--build', build, '--parallel', args.jobs, env=environment)
            run('cmake', '--install', build, env=environment)
    toolchain = subprocess.check_output(['dpkg-query', '-W', '-f=${binary:Package}=${Version}\n'])
    common = ((ROOT / 'packaging/static-linux-lock.json').read_bytes() +
              (ROOT / 'packaging/media-lgpl-lock.json').read_bytes() + toolchain +
              inspect.getsource(run_stage).encode())
    previous = b''
    for name, step, helper in [
        ('desktop', desktop, 'static-desktop-sdk.py'), ('util-linux', util_linux, None),
        ('pipewire', pipewire, 'static-pipewire.py'), ('media', media, None),
        ('sdl', sdl, None), ('model-https', model_https, 'static-model-https.py')]:
        extra = (ROOT / 'packaging' / helper).read_bytes() if helper else b''
        identity = hashlib.sha256(common + inspect.getsource(step).encode() + extra + previous).hexdigest()
        run_stage(step, prefix / '.cast-static-stamps' / name, identity)
        previous += identity.encode()
