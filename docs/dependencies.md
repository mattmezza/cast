# Dependencies, licensing and distribution

The project-owned C code, original bitmap glyphs and documentation use the MIT
license in [LICENSE](../LICENSE). The bundled glyphs were authored for cast and
contain no externally licensed font data. The vendored [inih](../vendor/inih)
parser is release r60 and BSD-3-Clause; its unmodified license and source are
shipped with each installation. This permissive code is compatible with the
licenses of the dynamically linked libraries below.

| Dependency | Purpose | License/source |
|---|---|---|
| inih r60 | INI parsing with source-line callbacks | BSD-3-Clause, bundled LICENSE.txt |
| FFmpeg libavcodec/libavformat/libavutil/libswscale/libswresample | MJPEG/local logo decode, conversion, software encoding, audio resampling and muxing | LGPL-2.1-or-later baseline; GPL build options can change the effective license |
| Fontconfig | Resolve configured system font patterns and fallback glyphs | Distribution COPYING (MIT/permissive notices), installed with cast |
| FreeType | Rasterize pause/blur text and cached static overlays | FreeType License (FTL), BSD-style credit clause, compatible with GPLv3 |
| PipeWire | Audio capture/mix source and optional portal video | MIT for library core; distribution package carries notices for its other components |
| Xlib, Xext, RandR, Xi, Xfixes, XComposite | Optional Xorg capture/input/presentation | MIT/X11 family licenses from the distribution packages |
| GLib/GIO/GObject | Optional Wayland async D-Bus portal and FD passing | LGPL-2.1-or-later |
| v4l2loopback kernel module | Existing virtual camera device | GPL-2.0 kernel module; independently installed, not bundled or linked into cast |

Portions of this software are copyright © 1996–2026 The FreeType Project
(www.freetype.org). All rights reserved. The unmodified FreeType and Fontconfig
notices are installed under `share/licenses/cast`; these libraries remain dynamic.
See [FreeType's official license choice](https://freetype.org/license.html).
Noto Sans is resolved from the system; release packages depend on Arch's noto-fonts,
which carries its own font licenses. No system font files are copied into cast.

FFmpeg's [official licensing page](https://ffmpeg.org/legal.html) explains that
GPL-enabled components change FFmpeg's effective license. The development
machine's FFmpeg 9.0.2 was built with `--enable-gpl --enable-version3` and reports
GPL-3.0-or-later. The resulting binary uses those system libraries; the project
source remains MIT, while redistribution of that linked binary must satisfy the
applicable GPL terms. MIT and BSD-3-Clause permit that combination. For GLib/GIO,
see the [official API license declaration](https://docs.gtk.org/gio/index.html).
GPLv3 and LGPLv2.1 license texts are included in `licenses/` and installed with
the package. Libraries are dynamically linked and are not copied into the binary
archive; install the listed distribution dependencies first.

`make package` creates a local ready-to-run Linux binary archive, a matching cast
source archive and checksums. The binary archive's `build-info.txt` identifies
architecture, selected backends and the resolved dynamic libraries. The source
archive contains build scripts, all project/vendor source, tests and notices.
These archives are built for this machine's ABI, not universal static binaries.
Distributing GPL-linked binaries to others also requires providing complete
corresponding source for the exact dependency builds under their licenses;
obtain it from your distribution's packaging/source repositories. The local package target does not fetch dependency source or publish archives.
The GitHub release target additionally collects the exact pinned Arch FFmpeg and
GLib source trees, patches, upstream licenses and package/build metadata into a
dependency-source asset. The collector verifies Arch source hashes and upstream
commit pins and rejects changed installed package versions. This asset identifies
its scope explicitly; separately packaged codecs, PipeWire, X11 and other transitive
dependencies are not bundled. See [release instructions](releases.md). Inspect `ffmpeg -L` and
your distribution package metadata before making a redistributable release.

Build tools are a C compiler, GNU make and pkg-config. Python is used only for
integration tests; no Python interpreter, test fixtures, compiler or privileged
input device is required for normal runtime. Xorg dependencies are absent with
`X11=0`; portal GLib dependencies are absent with `WAYLAND=0`. PipeWire audio and
FFmpeg, Fontconfig and FreeType remain required in either build. A usable system
font is required for output text, with Noto Sans as the default. Headless builds have no GUI toolkit dependency. `PANEL=1` adds the optional Clay
layout library (vendored v0.14, MIT), SDL3 and SDL3_ttf (zlib licenses). The bundled
Inter panel font is SIL OFL 1.1, with its unmodified license installed alongside
Clay and SDL3_ttf notices. The panel is C and does not introduce a browser runtime.
The development machine uses a locally built static SDL3_ttf 3.2.2 SDK because
the system package is absent; system SDL3, FreeType and HarfBuzz remain dynamic.
Standard builds use the distribution's SDL3_ttf package.
