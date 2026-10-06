# CPU speech SDK provenance

Speech is private Pro code and an optional `STT=1` build input. Ordinary Community
and Pro builds never download SDK sources or models. The shared private speech
service uses a separate helper; captions and notes retain distinct consent/epoch
boundaries. Model licensing and acceptance are separate from SDK licensing.

The explicit offline builder is:

```sh
python3 tools/deps-stt.py --print-plan
python3 tools/deps-stt.py --source-git /absolute/path/offline-whisper-cache \
  --output build/deps-stt --jobs 2
```

The cache must contain whisper.cpp 1.9.4 commit
`927cfce34f31707e17f2bff35c349632fb9e2c3a`. This pin bundles ggml 0.23.0. The
versions and options are checked against [upstream whisper CMake](https://github.com/ggml-org/whisper.cpp/blob/927cfce34f31707e17f2bff35c349632fb9e2c3a/CMakeLists.txt)
and [bundled ggml CMake](https://github.com/ggml-org/whisper.cpp/blob/927cfce34f31707e17f2bff35c349632fb9e2c3a/ggml/CMakeLists.txt).
The full upstream tree and license notices are preserved; models are not fetched.

The initial recipe supports Linux x86_64. It builds four static SDK archives with
the bundled generic CPU backend: whisper, ggml, ggml-base and ggml-cpu. It disables
native CPU tuning, optional x86 instruction extensions, OpenMP, GPU/BLAS/RPC
backends, dynamic backend loading, download support, external ggml, examples,
servers and dependency fetches. These are SDK archives, not a promise of a fully
static Cast executable. System C/C++ runtime closure is still audited separately.

The builder verifies cached Git objects and materializes the exact pinned commit.
It records SHA-256 hashes of the source archive, static libraries, lock and recipe;
preserves CMake cache, commands/compiler flags, complete source and upstream license
texts in `corresponding-sources.tar.gz`; and compiles/runs a CPU-only backend smoke
program. The generated `prefix/lib/pkgconfig/whisper.pc` includes the entire static
archive group so no ambient ggml backend enters the helper. `profile.json` records
the exact pkg-config flags and empty external CPU backend path.

A source build starts with `release_eligible=false`. Local testing requires
`STT_ENGINEERING=1`. Official verification requires an acceptance record bound to
the exact SDK identity, with reviewed inference/model, signed-helper boundary,
full-application, source/license inventory and runtime-closure evidence. Supply
real acceptance files and their hashes through
`packaging/speech-profile.py qualify --profile ... --review-record ...`; qualification
checks the same immutable evidence before changing eligibility. Merely changing a
manifest boolean cannot qualify an installed engineering SDK. Review evidence is
a local release input, not a substitute for production signing keys or the other
release gates.

The review record has schema 1, `reviewed_by`, `sdk_sha256` (SHA-256 of the canonical
JSON SDK fields used by `sdk_identity`), and `acceptance`. That object has exactly
`cpu_model_acceptance`, `signed_helper_boundary`, `full_application`,
`source_license_inventory` and `runtime_closure`, each with an actual `path`,
`sha256` and `passed:true`. SDK identity covers the pin, source archive, recipe/lock,
versions, flags, library inventory, configured options, smoke result and source kit.
All files are rehashed by official verification. No production record is supplied
by this repository.

The current local engineering profile was generated separately using installed
whisper.cpp 1.9.4-dev with external ggml 0.25.3 and one explicit CPU backend:

```sh
python3 packaging/speech-profile.py candidate \
  --cpu-backend /usr/lib/ggml/libggml-cpu-alderlake.so \
  --output build/deps-stt-candidate
```

That profile hashes the complete observed dynamic closure, copies available license
texts and records unshipped host dependencies. It is CPU-only, requires an explicit
engineering flag, and always fails official verification. The helper loads only
the specified CPU backend, never all installed backends, and disables GPU inference.
MIT covers the whisper/ggml SDK; GCC runtimes use the GCC runtime exception and
glibc uses LGPL. Unknown licenses, changed files, different compiler/linker flags,
unresolved libraries and accelerator libraries fail validation.

The controlled upstream Git cache is absent in this sandbox, so the pinned source
build and its inference acceptance have not run. The builder plan, missing-cache
failure and profile tamper/qualification guards were tested with explicit
nonproduction structural fixtures. Current signed application tests use the
installed engineering SDK; they do not establish production source provenance.
