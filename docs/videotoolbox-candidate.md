# Opt-in macOS VideoToolbox engineering candidate

The normal controlled LGPL SDK and `auto` encoder remain software based.
VideoToolbox is an explicit engineering option, not a runtime-qualified or
release-qualified configuration. No 4K30 guarantee has been established.

On a native macOS build host, choose a **new, nonexistent SDK root**:

```sh
python3 tools/deps-lgpl.py --hardware videotoolbox \
  --root "$PWD/build/deps-lgpl-videotoolbox-candidate" --jobs 4
make -f Makefile.macos MEDIA_PROFILE=lgpl \
  LGPL_ROOT="$PWD/build/deps-lgpl-videotoolbox-candidate"
```

The candidate command refuses existing destinations before downloading or changing
files. It uses the same hash-pinned source archives and shared LGPL configuration,
adds only `--enable-videotoolbox` and `--enable-encoder=h264_videotoolbox`, and records
the Apple system-framework integration in `profile.json`. Its release eligibility
is false. The ordinary builder remains destructive when explicitly rebuilding an
existing ordinary SDK; do not point it at a working SDK to experiment.

Select `record.video_codec=h264_videotoolbox` (or
`stream.video_encoder=h264_videotoolbox`) explicitly. Use `record.rate_control=auto`
or `bitrate`, not `crf`. The application requires hardware encoding (`allow_sw=0`,
`require_sw=0`) and requests realtime operation. Initialization errors are returned;
this explicit selection never silently falls back to CPU encoding. Registry
availability does not prove that the current hardware supports requested dimensions.

The current input remains composited CPU RGBA converted to CPU YUV420P, which FFmpeg
uploads into VideoToolbox pixel buffers. This is hardware encoding, not zero-copy
capture or GPU composition. Auto selection continues to use its reviewed CPU order.

Before qualification, test synthetic 3840x2160 at 30 fps with decode/count, duration,
resolution, text/detail quality, bitrate, latency and queue/backpressure checks;
then measure the full capture/compositor path on each supported Mac generation.
Check actual hardware use and reject software fallback. Audit relocated framework
and library dependencies, corresponding sources, signatures, and replacement behavior.
The candidate manifest deliberately cannot pass official release checks.
