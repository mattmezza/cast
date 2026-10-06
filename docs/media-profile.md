# Controlled shared media profile

`make deps-lgpl` is an explicit network operation. It builds pinned upstream
OpenH264, OpenSSL, zlib and FFmpeg sources into `build/deps-lgpl/prefix`; ordinary
`make` never downloads dependencies. `tools/deps-lgpl.sh --offline` requires all
four verified archives in `build/deps-cache`. Set `CAST_DEPS_CACHE` or `--cache` to
use another cache, and `--root` to select a separate build directory. A mismatch
fails before extraction/build and never replaces an existing cache entry. Archives
and all copied license texts, exact configure/build argv and the empty patch
manifest are recorded alongside the shared libraries. FFmpeg's corresponding source
bundle includes those original archives and build materials; it contains no private
Cast Pro application source.

The authoritative pins, upstream URLs, SHA256 hashes, copyright choices and explicit
component allowlist are in [media-lgpl-lock.json](../packaging/media-lgpl-lock.json).
They are FFmpeg 9.0.2, OpenH264 2.6.0, OpenSSL 3.5.4 and zlib 1.3.1. Pins make the
build repeatable; they do not assert current vulnerability clearance. Production
release review must check the selected versions and refresh the lock when needed.

FFmpeg is shared, LGPL-3.0-or-later, with GPL and nonfree components disabled and
external autodetection disabled. LGPLv3 is selected explicitly for the reviewed
Apache-2.0 OpenSSL backend. The build includes H.264 software encoding through
BSD-2-Clause OpenH264, native AAC, FLAC/PCM/FFV1/rawvideo, MJPEG and PNG encoding,
MJPEG/H.264/AAC plus PNG/JPEG/WebP/BMP decoding, swscale and swresample,
MP4/Matroska/FLV/WebM/WAV muxers, and file/pipe/TCP/TLS/RTMP/RTMPS protocols.
Dependencies selected internally by those components also appear in FFmpeg's
recorded configure result. There is no hardware SDK, GPL codec or model dependency. The JPEG implementation
retains its original notices: this software is based in part on the work of the
Independent JPEG Group; those FFmpeg JPEG source files have no local modifications.
WebM is a muxer option; H.264/AAC cannot be put into WebM. An explicit encoder absent
from this profile returns an availability error. Community system builds continue
to expose their installed encoders.

OpenSSL uses system CA trust at `/etc/ssl` and the configured optional stream CA
file. Cast requests `tls_verify=1` and the intended hostname. The pinned FFmpeg
OpenSSL code checks hostname/IP subject alternative names, suppresses SNI for
numeric hosts and verifies certificates. TLS transport integration tests must still
run against the actual resulting distribution before release.

Self-building OpenH264 under its BSD source copyright license does **not** provide
Cisco's patent coverage for Cisco-distributed binary modules. Exact commercial
H.264/AAC and SDK distribution needs owner/legal review. Cast does not download a
Cisco binary or purchase a codec license. FFmpeg has no commercial-license buyout.
See [FFmpeg legal guidance](https://ffmpeg.org/legal.html),
[OpenH264 source license](https://github.com/cisco/openh264/blob/v2.6.0/LICENSE) and
[Cisco binary conditions](https://www.openh264.org/BINARY_LICENSE.txt).

Recording and streaming share `src/media_codec.c`. `auto` initializes candidates,
choosing x264 then OpenH264 in a system Community build and OpenH264 in the LGPL
profile. A failed first candidate is reported with the selected encoder. Explicit
encoder names never substitute another encoder. Supported CPU YUV420P, codec
initialization and unconsumed encoder AVOptions are checked before returning
success. Hardware names only work when their actual context initializes and accepts
the implemented CPU-frame path; no advertised hardware codec is presumed usable.

The same layer performs RGBA-to-encoder conversion. For an LGPL-profile canvas of
at least 1280x720 it uses four bounded libswscale slice workers through the supported
frame API. It retains the original fast-bilinear filter, chroma format, dimensions,
color matrix and range; it changes neither quality settings nor source cadence.
Conversion completes synchronously before the worker continues its existing privacy
epoch checks. No borrowed queue pixels survive the call. System Community builds
retain their original single-thread conversion path. No new library is required.

`record.rate_control=auto` retains CRF/preset semantics for x264/x265 and uses
`record.bitrate_kbps` otherwise. Lossless codecs such as FFV1 retain their lossless
mode with auto and report bitrate/CRF/preset inactive; an explicit bitrate mode is
rejected for them. `bitrate` explicitly chooses the neutral bitrate target;
OpenH264 may vary around that target and codec frame skipping is disabled. `crf` requires x264/x265 and fails on OpenH264. `record.crf` is never converted into
an allegedly equivalent OpenH264 quality. `stream.video_encoder=auto` uses the
existing `stream.video_bitrate_kbps`; the stream preset is active only for x264.
The selected codec and inactive legacy settings appear in recording start replies
and `cast doctor`. Legacy explicit x264 INI settings remain schema-valid but fail
availability/start on a tree without x264. Use the explicit `cast config migrate`
command to inspect suggested changes; startup never rewrites INI files.

`cast doctor` reports the licenses, paths of all five actually loaded FFmpeg
libraries, the FFmpeg configure string and initialized encoder readiness. Pro
validates the loaded libraries before acquiring capture/audio, rejecting GPL or
nonfree configurations. Runtime checks do not hash libraries: compatible modified
LGPL shared libraries remain replaceable. Release provenance checks separately
verify the pinned originals and packaged dependency closure.

## Local verification on 2026-10-05

The pinned FFmpeg source archive was available locally and its SHA256 matched the
lock. The complete source-built profile could not run: sandbox DNS denied upstream
downloads, and the pinned OpenH264/OpenSSL/zlib source archives were missing. The
`--offline` command returned the exact missing OpenH264 archive and expected hash.

A distinct **nonrelease** candidate tree was built at
`build/deps-lgpl-candidate/prefix` using the pinned FFmpeg source and installed
OpenH264 2.6.0, OpenSSL 3.6.5 and zlib 1.3.2. All five loaded FFmpeg libraries were
verified to resolve to this LGPLv3 candidate. The external codec/TLS/compression
libraries resolved to the system, so this candidate does not pass the controlled
source/provenance contract and is not an official Pro dependency tree.

The real media suite passed with OpenH264: actual JPEG/MJPEG, BMP and lossless WebP
image decode (plus PNG through the visual suite), recording/audio decode, removed cut time,
solid pause and decoded silence, independent output privacy epochs, bounded queues,
responsive controls, recording failures/recovery, MJPEG colors and camera worker
barriers. The production stream codec/cadence/FLV packet paths also encoded a file
with 30 decoded H.264 frames, AAC and a privacy epoch keyframe at the midpoint.
MP4 H.264/AAC and Matroska FFV1/FLAC also completed real encode/decode roundtrips,
preserving explicit lossless Community recording choices.
That file test does not verify network transport. Existing RTMP/RTMPS/sustained
network tests could not run because the sandbox denies creating an AF_INET socket;
`tests/test_stream.c` fails its `free_port` fixture before starting a worker.

The exact candidate build and test logs remain in `build/deps-lgpl-candidate` and
`build/media-lgpl-test*.log`. No physical capture or commercial service acceptance
was claimed. The original single-thread conversion candidate failed a 60-second
1920x1080@30 recording: 1800 submitted frames, 1435 H.264 packets, 365 drops and
60.021-second duration. Profiling found approximately 28 ms conversion plus 11 ms
encoding per frame, exceeding the 33.33 ms frame budget. Four encoder threads/four
slices alone had improved the earlier 1373-packet result without solving conversion.
The original 1280x720@30 test passed: 1800 submitted, 1799 packets, one drop,
60.021 seconds; average conversion 14.999 ms and encode/mux 5.765 ms.

After enabling supported four-thread conversion, the same real 60-second
1920x1080@30 moving-gradient test passed: **1800 submitted, 1800 H.264 packets,
zero drops, 60.021 seconds**, with the unchanged 6000 kbit/s target and AAC audio.
Average conversion was 13.331 ms and encode/mux 9.856 ms. The acceptance remains
at least 1750 packets and at most 50 drops. The artifact is
`/tmp/cast-lgpl-1080p60s-threaded-conversion.mkv`; the matching `.log` records the
result. A brief concurrent short quality test did not cause a drop. This is a CPU
worker throughput result, without physical capture or network transport.

The media regression suite also compared every Y/U/V output byte against the
original scaler for random padded inputs, resized inputs, changing dimensions and
both recording and streaming color matrices. Those comparisons were exact; the
destination retained no converter references. The original codec/container,
privacy epoch and responsive-control checks passed with the new converter.
Both system Community and LGPL-candidate media suites passed, as did the updated
address/undefined sanitizer suite with the existing `detect_leaks=0` limitation.
Independent `ffprobe` decoding counted all 1800 moving-test frames at exactly
1920x1080 (`/tmp/cast-lgpl-1080p60s-threaded-decode.json`).

The independent Pro quality fixture decoded 90/90 full-resolution frames at 30 fps
with zero drops. Fixed quality thresholds passed: minimum Y/U/V PSNR was
37.408/47.156/47.458 dB, fine-stripe contrast retained 96.17%, and text-region mean
absolute luma error was 2.3896. A deliberately reduced 720p source enlarged to
1080p failed the same thresholds (20.402 dB luma and 1.23% fine-stripe contrast),
so merely labeling an enlarged frame 1080p would not pass. The encoder context
retained its 6000000 bit/s target; this static fixture's measured video payload was
1.110 Mbit/s because OpenH264 bitrate varies with content. It does not claim that
a 6 Mbit/s target forces 6 Mbit/s output. The complete report and artifact hashes
are in `/tmp/cast-current-pro-1080-quality-report.txt`; the fixture is
`tests/test_media_quality.c`.

The test host reports Intel 8086:7D41 with the i915 driver in sysfs, but `/dev/dri`,
NVIDIA and KFD device nodes are absent from this environment. No GPU encoder could
be initialized or benchmarked. Its Intel Core Ultra 5 235U exposes 14 logical CPUs
and AVX2; the task's cgroup has no explicit CPU quota. NASM/YASM are unavailable,
so candidate FFmpeg still has `--disable-x86asm`. The controlled source builder
requires NASM on x86 and enables OpenH264 assembly; that complete pinned optimized
build, RTMP/RTMPS trust/hostname checks and clean-image release dependency closure
remain unverified production work. Passing CPU throughput does not make this
candidate release-eligible.

Reproduce the source-cache preflight with:

```sh
make deps-lgpl DEPS_ARGS=--offline
python3 tests/test_deps_lgpl.py
```

After a verified full dependency build, run the real checks using the actual tree:

```sh
make EDITION=community MEDIA_PROFILE=lgpl X11=0 WAYLAND=0 PANEL=0 check-unit
make EDITION=community MEDIA_PROFILE=lgpl X11=0 WAYLAND=0 PANEL=0 check-media-sustained
make EDITION=community MEDIA_PROFILE=lgpl X11=0 WAYLAND=0 PANEL=0 check-stream-sustained
```

The network checks require local TCP sockets and the existing test-only TLS tools.
These commands are acceptance procedures; the full pinned-profile commands above
were not executed successfully in the restricted environment described here. A
nonrelease candidate can be explicitly selected with
`LGPL_ROOT=/absolute/path/to/build/deps-lgpl-candidate`; it remains ineligible for
an official release. The sanitizer run used address/undefined sanitizers with
`ASAN_OPTIONS=detect_leaks=0` because the existing full media harness loads PipeWire
modules with external exit allocations; a complete dependency leak check is not
claimed.
