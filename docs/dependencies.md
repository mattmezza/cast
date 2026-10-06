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
| FreeType | Rasterize pause/blur text, static overlays and native preview Inter | FreeType License (FTL), BSD-style credit clause, compatible with GPLv3 |
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
input device is required for normal runtime. Cast introduces no direct Xorg dependency with
`X11=0` (distribution FFmpeg libraries may still pull Xorg transitively); portal GLib dependencies are absent with `WAYLAND=0`. PipeWire audio and
FFmpeg, Fontconfig and FreeType remain required in either build. A usable system
font is required for output text, with Noto Sans as the default. Headless builds have no GUI toolkit dependency. `PANEL=1` adds the optional Clay
layout library (vendored v0.14, MIT), SDL3 and SDL3_ttf (zlib licenses). The bundled
Inter UI font is SIL OFL 1.1. Xorg preview also uses the embedded Inter font
through the existing FreeType dependency, including with `PANEL=0`. Its unmodified
license is installed whenever the panel or Xorg preview is built. The panel is C and does not introduce a browser runtime.
The current development environment provides SDL3 3.4.16 and SDL3_ttf 3.2.2
through pkg-config; normal builds use the distribution packages. An earlier local
static SDL3_ttf SDK remains an explicit development option.

Streaming reuses FFmpeg H.264/AAC/FLV and RTMP/RTMPS support; it adds no runtime
library. Test-only local TLS fixtures require the `openssl` command, and native
panel integration uses Xvfb, xdotool and xclip. No such test tools are needed to run Cast.

## Edition profiles and complete artifact accounting

Public Cast code remains MIT. Community `MEDIA_PROFILE=system` can link a
GPL-enabled distribution FFmpeg; redistributing that combination carries its GPL
obligations. Pro requires the separately built controlled shared LGPL media tree.
Disabling x264 in application settings does not change a GPL FFmpeg's license.
FFmpeg offers no proprietary commercial buyout; see its
[primary licensing guidance](https://ffmpeg.org/legal.html).

The controlled build pins FFmpeg 9.0.2, OpenH264 2.6.0, OpenSSL 3.5.4 and zlib 1.3.1
with upstream URLs and verified SHA-256 in `packaging/media-lgpl-lock.json`.
`tools/deps-lgpl.sh` is explicit opt-in downloading/building; `--offline` accepts
only complete verified caches. It preserves configuration/patch/license/build
materials and library hashes. Autodetection, GPL/nonfree and unused external codec
families are excluded. The required MP4/MKV, FLV, H.264/AAC, MJPEG, configured local
logo formats, conversion/resampling and RTMP/RTMPS/TLS are retained. OpenH264 is the
software baseline candidate; actual encoder initialization and sustained recording/
streaming checks, including decode verification, determine technical readiness.

A local candidate profile may validate engineering behavior with installed libraries
when exact source caches are unavailable. Candidate builds have nonproduction
identity and are ineligible for official release. Missing pinned source, unknown
transitive licenses or absent clean-runtime acceptance remain unresolved production
inputs; they are never treated as clearance by a successful compiler invocation.

OpenH264 source has BSD copyright terms. A self-built/bundled codec does **not**
inherit Cisco-provided binary patent coverage: that coverage has specific separate
binary delivery/use conditions. This project neither downloads Cisco binaries nor
buys a commercial x264 license. H.264/AAC and optional SDK patent/distribution
terms require review for the exact commercial offering. See
[Cisco's binary conditions](https://www.openh264.org/BINARY_LICENSE.txt).

Pro-only license/update verification uses libsodium 1.0.22 under ISC. Community
has no direct crypto/model dependency. The actual system shared package version
is recorded by the artifact audit; production must supply pinned crypto source/build
provenance, rather than pretend an arbitrary installed library is the pinned release.

`packaging/audit.py inventory` walks the resolved ELF dependency closure and records
library paths/hashes, package/version/evidence, chosen library-level license,
static/shared linkage, distributed status, source/build references and full available
license texts. `make check-licenses` writes `inventory.json`,
`THIRD-PARTY-NOTICES.txt` and `license-texts/` under the selected build directory.
The native text closure includes Graphite2; its installed COPYING explicitly offers
LGPL-2.1-or-later, selected here with its SIL notice and full LGPL text. HarfBuzz
remains MIT. Package lists provide evidence; unresolved library choices are `NOASSERTION` and
fail strict Pro checking. FreeType deliberately selects FTL; Fontconfig retains its
HPND/Unicode notices; glibc shared runtime is LGPL and GCC shared runtime selects the
GCC exception rather than blanket-rejecting GPL package tools. Clay/inih and embedded
Inter are separately accounted; no installed Noto/system fonts are copied.

`packaging/runtime-dynamic.json` identifies dependencies outside ldd, including
PipeWire plugins, SDL backend loaders, graphics drivers and font selection.
Those modules require a clean-runtime load audit before production. The optional
private speech helper uses the explicit CPU SDK profile in [speech-profile.md](speech-profile.md);
models are installed separately. WebRTC and additional hardware encoder SDK modules
are not distributed today. Independently inventory code/model provenance and exact licenses; do not
assume whisper.cpp/ggml, Whisper models, ONNX/Silero, CUDA or other SDKs have one license.
System v4l2loopback stays independently installed and unbundled. Compiler, Python,
Xvfb, curl, NASM and TLS fixture tools are build/test inputs, not application assets.

For distributed controlled media libraries, include the matching dependency source
archive and build/patch materials beside the artifact. Pro packages put only reviewed
controlled shared media in `usr/lib/cast-pro/media`; they never overwrite system
FFmpeg, depend on Arch's GPL FFmpeg or bundle every library on the developer machine.
LGPL libraries remain ABI-replaceable without hash restrictions at runtime; original
private application source is not claimed to be LGPL corresponding source. The EULA
permits library replacement and debugging reverse engineering required by LGPL.

The previous Arch FFmpeg/GLib collector is a historical scoped Community helper. It
does not cover controlled Pro sources or the complete transitive system closure.
Exact relied-on system package sources/notices/build materials, MPL file-level changes
when selected and optional SDK terms require their own matching evidence. Official
Pro packaging fails closed on unreviewed/disallowed runtime entries, incomplete
corresponding sources, missing production keys/metadata or dynamic-runtime reviews.
Automated classification is engineering evidence, not legal sign-off.

See [single-binary delivery feasibility](static-delivery.md) for the current static
dependency audit and Pro LGPL relinking/source-material requirements. Run
`make check-static-deps` with the selected edition/backend/media variables to repeat
the read-only archive inventory; it does not download or build dependencies.
