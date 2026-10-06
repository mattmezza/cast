# Static and single-file delivery

The current Community and Pro binaries are dynamically linked. The native panel,
X11, Wayland, recording, audio and streaming are retained; no reduced headless binary
is presented as a substitute for the complete application.

## Current build inputs

On the implementation host, the development packages provide shared libraries but
not the static archives needed for Fontconfig, FreeType, FFmpeg, PipeWire, X11,
SDL3/SDL3_ttf, and libsodium. The controlled Pro FFmpeg recipe deliberately builds
shared libraries. Network access from the implementation session cannot obtain the
missing source SDKs. A fully static binary for either edition cannot currently be
built and verified from those inputs.

Run the read-only inventory with the same edition and backend selection as the build:

```sh
python3 tools/check-static-deps.py --edition community
python3 tools/check-static-deps.py --edition pro --lgpl-root build/deps-lgpl-candidate
python3 tools/check-static-deps.py --edition pro --stt 1 --lgpl-root build/deps-lgpl-candidate
```

`--x11`, `--wayland` and `--panel` accept `0` or `1`; all default to `1`.
`--stt 1` includes the optional Pro inference dependencies. The implementation-host
audit found 81 missing Community archives, 36 for Pro without inference, and 39
for Pro with inference. These are input audits, not successful static builds.
`--json` emits a machine-readable report. Exit status `2` means inputs are missing
or cannot be inspected. The inventory uses `pkg-config --static` and the compiler's
library search paths. It does not download dependencies, replace the current SDK,
change edition policy, or claim that archive presence alone proves a working build.

## Requirements that linking cannot remove

A self-contained userspace executable still needs a compatible Linux kernel and
desktop session. Virtual-camera output needs the independently installed
`v4l2loopback` kernel module and its device. Audio uses the running PipeWire server
and its modules. Wayland capture needs the desktop portal and compositor. Desktop
graphics drivers, network certificates, and composition-text fonts are runtime
resources. Bundled Inter covers the panel, not every selectable output font.

Linking a library statically does not automatically embed that library's dynamically
loaded modules, driver stack, or data files. A future static package must verify
those paths on a clean machine with its advertised capture/audio/panel features.
An AppImage or self-extracting shared-library bundle can be a single download but
is not a statically linked binary; neither artifact is currently produced here.

Pro speech processing also uses a separate structured helper process. Its model and
optional VAD assets are explicitly selected or downloaded; browsing a control never
downloads them. Statically linking the desktop executable would not turn this
isolated inference service and its model data into one complete binary. A future
single-file distribution would need a deliberate extraction/launch strategy and
its own clean-machine acceptance checks.

## Pro distribution materials

The private Pro implementation remains separate from the MIT Community sources.
Any future static Pro build using LGPL libraries needs the applicable corresponding
library sources and application code/materials allowing users to relink with a
modified library. This can include application object files without publishing the
private source. The current shared-library source bundle does not substitute for
those static relinking materials. See the [GNU LGPL static-linking explanation](https://www.gnu.org/licenses/gpl-faq.en.html#LGPLStaticVsDynamic)
and the [LGPLv3 combined-work requirements](https://www.gnu.org/licences/lgpl.html).

Neither a static SDK nor a static relinking package has been verified in this task.
No static release asset is advertised or silently replaced by a shared binary.
