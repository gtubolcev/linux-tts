# Packaging recipes

How the packages around the modules were built. These are the scripts that made the
local `~local` packages; they still use fixed paths on the build machine and will be
replaced by `debian/` directories for a PPA.

| Directory | Package | Upstream | What we change |
|---|---|---|---|
| `speech-dispatcher/` | speech-dispatcher 0.12.1-2ubuntu1+fix1 | Ubuntu source package | `reload-leaks.patch`: on a configuration reload (SIGHUP) the punctuation was spoken once per reload, audio connections leaked, PulseAudio did not reconnect. Added to `debian/patches/series`. Also in upstream master, not sent yet. |
| `rhvoice/` | rhvoice 1.14.0-3+voice3 | Ubuntu multiverse source package (has the voices) | `apply-voice-patch.py <src/sd_module>`: adds `RHVoiceVoice "<language>" "<voice>"` to the speech-dispatcher module, the voice used for a language when the client does not name one. `patches/sd-module-user-voices`: the module also loads languages and voices from `$XDG_DATA_HOME/RHVoice/{languages,voices}`, where xfce4-speech-settings installs them without root. |
| `sherpa-onnx/` | sherpa-onnx 2.0.1 | github.com/k2-fsa/sherpa-onnx tag v2.0.1 (af2d8d8) | `debian/patches/vulkan-provider.patch`: the provider "vulkan", which registers the onnx-vulkan-rs plugin (`libonnxruntime_ep_vulkan.so` next to libonnxruntime, or `SHERPA_ONNX_VULKAN_EP`). Built against the private onnxruntime of `onnxruntime-tts/` (RUNPATH); `make-source.sh` builds the PPA source package (the CMake dependencies from `deps.txt` go into the orig tarball, so the build needs no network). `debian/sherpa-say` is a small command line helper (model family detected from the files). |
| `onnxruntime-tts/` | libonnxruntime-tts, libonnxruntime-tts-dev, python3-onnxruntime-tts 1.32.0 | onnxruntime v1.32.0 = fab30a46 (github.com/microsoft/onnxruntime is gone since 2026-11-17: the commit as archived by Software Heritage on 2026-11-14, fetched by hash from a fork) | Nothing in the code. Private: `/usr/lib/<triplet>/linux-tts/` (library, `pkgconfig/libonnxruntime.pc`), `/usr/include/linux-tts/onnxruntime`; consumers set RUNPATH and `dh_shlibdeps -l`. The Python module (for silero-server) in `/usr/lib/python3/linux-tts/onnxruntime`, off sys.path; the program adds the directory. Distribution packages stay untouched. CPU only, no XNNPACK, no LTO, no dwz (too many DIEs). Dependencies in `mirror/` of the orig tarball as for `onnxruntime/`. |
| `onnxruntime-ep-vulkan/` | onnxruntime-ep-vulkan 0.0.1~git8625ef9 | github.com/automataIA/onnx-vulkan-rs 8625ef9 | `debian/patches/pad-modes.patch`: Pad modes edge, reflect, wrap (Supertonic). Rust 1.97 from the archive (`rustc-1.97`), crates vendored into the orig tarball (`make-source.sh`, `VENDOR_HOST=` runs `cargo vendor` on the build machine; `check-vendor.py` checks them against Cargo.lock). The plugin goes next to the private libonnxruntime. |
| `sherpa-onnx-models/` | sherpa-onnx-tts-* | sherpa-onnx `tts-models` release | `build-model-debs.sh`: one arch-independent package per model, files under `/usr/share/sherpa-onnx/tts/<name>/`. |
| `piper-tts/` | piper-tts 1.8.0 | github.com/OHF-Voice/piper1-gpl tag v1.8.0 (639388b) | `debian/patches/num-threads.patch`: onnxruntime picks the thread count (was fixed at 1), `PIPER_NUM_THREADS` overrides it; `local-espeak-ng.patch`: espeak-ng 212928b from the orig tarball instead of a git clone. `make-source.sh` builds the PPA source package (like sherpa-onnx's). Layout: `/usr/lib/piper-tts/{piper,libpiper.so,espeak-ng-data}`, command `piper-tts`. |
| `onnxruntime/` | libonnxruntime1, libonnxruntime-dev 1.23.2 (noble only) | github.com/microsoft/onnxruntime tag v1.23.2 | Superseded by `onnxruntime-tts/`: none of our packages build against it any more. Nothing in the code; for releases without onnxruntime (noble). CPU only, no Python, no XNNPACK, no LTO. `make-source.sh` puts the archives of the CMake dependencies named in `deps` (URL and SHA1 from the release's `cmake/deps.txt`) into `mirror/` of the orig tarball, where onnxruntime's CMake looks first. Upstream soname `libonnxruntime.so.1` (Debian's is `.so.1.23`). |

sherpa-onnx was first configured in an `ubuntu:26.04` container with `libonnxruntime-dev`:

```sh
cmake -S src -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr \
  -DBUILD_SHARED_LIBS=ON -DSHERPA_ONNX_ENABLE_PORTAUDIO=OFF \
  -DSHERPA_ONNX_ENABLE_WEBSOCKET=OFF -DSHERPA_ONNX_ENABLE_PYTHON=OFF \
  -DSHERPA_ONNX_ENABLE_TESTS=OFF -DSHERPA_ONNX_ENABLE_BINARY=ON \
  -DSHERPA_ONNX_BUILD_C_API_EXAMPLES=OFF
cmake --build build -j4
DESTDIR=$PWD/stage cmake --install build
```

## Model licences (what may go into a PPA)

| Model package | Licence | PPA |
|---|---|---|
| kitten-nano-en | Apache-2.0 | yes |
| vosk-tts-model-ru-0.9-multi | Apache-2.0 | yes |
| pocket | CC-BY-4.0 | yes |
| supertonic-3 | OpenRAIL-M (model), MIT (code) | to check: OpenRAIL-M has use restrictions |
| kokoro-tts-model-ru | OpenRAIL (kokoro-ru) | to check, as above |
| teratts-model-v2 | OpenRAIL-M + commercial attribution (TeraSpace); LICENSE and LICENSE-OPENRAIL must ship with it | to check, as Supertonic |
| mms-ru | CC-BY-NC-4.0 | no, non-commercial |
| Silero | CC BY-NC-SA 4.0 | no, installed by the user |
