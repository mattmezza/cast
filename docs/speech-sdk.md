# Needle/Whistle speech SDK evidence

`tools/deps-stt.py` imports an explicitly supplied **offline binary engineering
SDK**. It never downloads files, builds unavailable native sources, or qualifies
an official release. The pin is `packaging/speech-needle-lock.json`.

The [Needle repository](https://github.com/cactus-compute/needle) describes Whistle
speech recognition using the same engine as Needle's text models. The native C API
is shipped with the [Needle 3 artifacts](https://huggingface.co/Cactus-Compute/needle3/tree/d6c7d6a2e9655505e0f8e23339356dd1705900eb).
Speech needs the separate [Whistle model](https://huggingface.co/Cactus-Compute/whistle/tree/741118687e1f008f06d1351a402a731274789790),
not the text `needle3.cact`. These immutable revisions supply header/static
archive/license and model identities. Runtime C++ source and a reproducible native
build recipe were absent from the inspected public repository; a binary hash
establishes artifact identity, not source provenance or production qualification.

Place `libneedle.a`, `needle.h` and the upstream `LICENSE` in a local SDK directory,
then import them (all bytes must match the lock):

```sh
python3 tools/deps-stt.py --print-plan
python3 tools/deps-stt.py --sdk-dir /absolute/path/to/pinned-sdk \
  --model /absolute/path/to/whistle.cact --output build/deps-stt-needle
```

Only Linux x86_64 and macOS arm64 artifacts currently have reviewed identity pins.
`--platform` selects one explicitly; importing another platform is not evidence
that its helper was linked or tested. The optional model is hash-checked in place,
not copied or downloaded. Output must be new; an existing SDK is never overwritten.
The profile records hash-bound header, archive, license, lock and importer recipe.
It declares no corresponding source archive and always has `release_eligible=false`.

For the pinned Linux shared runtime, also supply the original offline wheel:

```sh
python3 tools/deps-stt.py --sdk-dir /absolute/path/to/pinned-sdk \
  --wheel /absolute/path/to/cactus_needle-3.3.3-py3-none-manylinux2014_x86_64.whl \
  --model /absolute/path/to/whistle.cact --output build/deps-stt-needle
```

The wheel and extracted `needle/libneedle3.so` each have an independent pinned
SHA-256. The imported name `libneedle.so` matches its SONAME. `readelf` checks the
five expected ELF runtime dependencies; `ldd` rejects unresolved dependencies.
The original wheel is retained as evidence. This variant bundles its C++ runtime
and uses an absolute SDK rpath for local engineering builds only. It is still
binary-only and cannot be qualified. The static macOS recipe remains separate.

The importer emits `prefix/lib/pkgconfig/needle.pc`. Use that directory in
`PKG_CONFIG_PATH`, or pass the exact `cflags` and `libs` from `profile.json` as
`STT_CFLAGS` and `STT_LIBS`. Pro speech builds require `STT_ENGINEERING=1`,
`STT_PROFILE` pointing to this profile, and `OFFICIAL_RELEASE=0`. No ambient ggml
CPU backend is involved. Both pinned archives require libc++; the Linux archive references `std::__1`
and additionally links libc++abi. A C++ compiler defaulting to libstdc++ is not
sufficient without those explicit runtime libraries. Runtime packages must exist
locally; the importer installs no dependencies.

The verifier rejects changed artifacts, unsupported platforms, extra archives,
flags differing from the recorded profile, source-provenance claims, missing
engineering mode and all official delivery attempts. `qualify` cannot promote
this binary-only profile. The static archive does not establish the final
helper's dynamic runtime closure; that needs separate platform/runtime review.
Preserved Apache-2.0 notices do not replace that review or model acceptance tests.

`tools/deps-stt-whisper.py` preserves the historical Whisper/ggml source recipe
byte-for-byte so existing source/review evidence remains inspectable. Its lock
and controlled-source verifier remain separate from Needle's engineering path;
Whisper qualification cannot qualify Needle binaries. Structural tests use
explicit fake local artifacts and are not inference or release evidence.

For existing Whisper configurations, stop the daemon and review an explicit
migration before starting the new build:

```sh
python3 ../cast-pro/pro/tools/speech-migrate.py /absolute/path/to/cast-pro.conf
python3 ../cast-pro/pro/tools/speech-migrate.py /absolute/path/to/cast-pro.conf --write
```

The first command previews the changes. The second preserves an exclusive
`.pre-needle` backup, disables transcription and clears the old model selection;
it does not download a model or turn on a microphone. Other settings are preserved.
Choose Whistle explicitly after restarting. Existing supported language selection
is retained; unsupported legacy engine settings are listed in the preview.

After importing the SDK, a local workspace build uses its matching pkg-config file
automatically (override `LGPL_ROOT` and `PRO_TRUST_HEADER` for your existing setup):

```sh
make -j4 EDITION=pro MEDIA_PROFILE=lgpl CUI=1 PANEL=0 STT=1 STT_ENGINEERING=1 \
  STT_HELPER="$PWD/build/cast-pro-stt-helper" \
  STT_HELPER_PATH="$PWD/build/cast-pro-stt-helper"
```

Linux and macOS use the same options; macOS currently has an arm64 SDK pin.
The explicit helper paths let `./cast-pro` run without installing into `/usr/local`.
These development builds do not change your license trust configuration.
