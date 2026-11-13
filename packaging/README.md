# Packaging recipes

How the packages around the modules were built. These are the scripts that made the
local `~local` packages; they still use fixed paths on the build machine and will be
replaced by `debian/` directories for a PPA.

| Directory | Package | Upstream | What we change |
|---|---|---|---|
| `speech-dispatcher/` | speech-dispatcher 0.12.1-2ubuntu1+fix1 | Ubuntu source package | `reload-leaks.patch`: on a configuration reload (SIGHUP) the punctuation was spoken once per reload, audio connections leaked, PulseAudio did not reconnect. Added to `debian/patches/series`. Also in upstream master, not sent yet. |
| `rhvoice/` | rhvoice 1.14.0-3+voice1 | Ubuntu multiverse source package (has the voices) | `apply-voice-patch.py <src/sd_module>`: adds `RHVoiceVoice "<language>" "<voice>"` to the speech-dispatcher module, the voice used for a language when the client does not name one. |
| `sherpa-onnx/` | sherpa-onnx 2.0.1 | github.com/k2-fsa/sherpa-onnx tag v2.0.1 (af2d8d8) | Nothing in the code. Built against Ubuntu's libonnxruntime with the flags below; `make-source.sh` builds the PPA source package (the CMake dependencies from `deps.txt` go into the orig tarball, so the build needs no network). `debian/sherpa-say` is a small command line helper (model family detected from the files). |
| `sherpa-onnx-models/` | sherpa-onnx-tts-* | sherpa-onnx `tts-models` release | `build-model-debs.sh`: one arch-independent package per model, files under `/usr/share/sherpa-onnx/tts/<name>/`. |
| `piper-tts/` | piper-tts 1.8.0 | github.com/OHF-Voice/piper1-gpl (639388b) | `num-threads.patch`: onnxruntime picks the thread count (was fixed at 1), `PIPER_NUM_THREADS` overrides it. |

sherpa-onnx was configured in an `ubuntu:26.04` container with `libonnxruntime-dev`:

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
