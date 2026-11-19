# How the pieces fit together

From the application down to the hardware. There are three kinds of layer:
the speech-dispatcher modules, the libraries that turn text into model
inputs (the front end), and the runtimes that run the neural network.

```
            application (Orca, Firefox, spd-say, ...)
                           |
                    speech-dispatcher
                           |
  +---------------+---------+-------------+------------+-----------+
  |               |         |             |            |           |
sd_sherpa-onnx  sd_kokoro  sd_teratts   sd_vosk     sd_silero   sd_rhvoice,
(+ -vulkan-rs:    |         |             |            |         espeak-ng
 one binary,      |         |             |            |           |
 2 names)         |         |             |            |           |
  |               v         v             v            v           v
  |   phonemizer: our C code: text -> phonemes      silero-     no neural
  |   espeak-ng   (espeak-ng, dictionaries)          server     network:
  |   (Piper),        |         |             |     (Python)    rule-based
  |   sherpa front    |         |             |        |        synthesis,
  |   end (others)    |         |             |        |        nothing to
  v                   |         |             |        |        accelerate
sherpa-onnx           |         |             |        |
(model families)      |         |             |        |
  |                   |         |             |        |
  +---------+---------+---------+-------------+        |
            |                                          |
            v                                          v
   +-------- ONNX Runtime (C) ----------+         ONNX Runtime
   | CPU execution provider             |        (Python, CPU)
   | + Vulkan plugin EP (onnx-vulkan-rs,|
   |   needs ONNX Runtime >= 1.29):     |
   |   the *-vulkan-rs synthesizers     |
   +------------------------------------+
```

Not packaged: `extra/zonos3` (ZONOS3-Mini on ggml/Vulkan, until its model
stops inventing words). Pied's `piper-generic` (the `piper` command) is
replaced by the Piper voices of `sherpa-onnx`.

| Module | Models | Front end | Runtime | Graphics card |
|---|---|---|---|---|
| `sd_sherpa-onnx` (synthesizers `sherpa-onnx`, `sherpa-onnx-vulkan-rs`) | Supertonic 3, Piper voices (`vits-piper-*`: piper-voice-* packages, the downloader), MMS, Pocket TTS | sherpa-onnx; for Piper espeak-ng in `sd_sherpa.c`, as piper-phonemize | ONNX Runtime | Piper, MMS, Supertonic: `-vulkan-rs`; Pocket: no |
| `sd_kokoro` | Kokoro (Russian) | `kokoro/ru-g2p.c`, espeak-ng | ONNX Runtime | not yet; the same plugin would work |
| `sd_teratts` | TeraTTS | `teratts/`, stress dictionary | ONNX Runtime | not yet, as above |
| `sd_vosk` | Vosk TTS | `vosk/vosk.c` | ONNX Runtime | not yet, as above |
| `sd_zonos3` (`extra/`, not packaged) | ZONOS3-Mini | zonos3.cpp | ggml | Vulkan (`Zonos3Gpu`) |
| (none yet) | kokoro.cpp | kokoro.cpp | ggml | Vulkan; a candidate, not tested |
| `sd_silero` | Silero v5 (user-installed, non-commercial) | silero-tts-onnx | ONNX Runtime (Python) | no |
| `sd_rhvoice`, espeak-ng | RHVoice voices, espeak-ng | built in | no neural network | not needed |

## One module, two synthesizers

speech-dispatcher starts a synthesizer `<name>` as the program
`sd_<name>` with the config `modules/<name>.conf`. `sd_sherpa-onnx` is
installed once; `sd_sherpa-onnx-vulkan-rs` is a link to it:

| Synthesizer | Config | Families | Provider |
|---|---|---|---|
| `sherpa-onnx` | `sherpa-onnx.conf` | supertonic piper pocket vits | cpu |
| `sherpa-onnx-vulkan-rs` | generated from it | piper vits | vulkan |

Both are in speech-dispatcher-sherpa-onnx; speech-dispatcher-piper-sherpa-onnx
only brings the packaged Piper voices. Until 2026-11-16 Piper was a
synthesizer of its own, `piper-sherpa-onnx` (a further link with
`PiperFamilies "piper"`); the module still reads options with the prefix
`Piper` (`PiperVoice`), so such a config keeps working. A `-vulkan-rs`
synthesizer fails to start without the plugin, and speech-dispatcher then
does not list it. The names say what runs under them (sherpa on ONNX
Runtime on onnx-vulkan-rs), leaving room for `sherpa-ncnn` or others later.

Up to `SherpaMaxModels` (2) models stay loaded, the one used longest ago
goes first: a text in two languages spoken by voices of two models does not
load a model at every change of language.

## Phonemizers

What turns the text into the model's input, per voice family:

| Voices | Text normalization (numbers, abbreviations) | Stress | Phonemes |
|---|---|---|---|
| Piper | espeak-ng (its own number rules) | espeak-ng | espeak-ng, Piper's data (`piper-espeak-ng-data`), clause by clause as piper-phonemize; called from `sd_sherpa.c`. Voices with `phoneme_type "text"` (letters) are not supported yet |
| Supertonic 3 | none: digits and Latin letters go to the model as they are, so "2026 году" is read wrongly | none (the model) | none: characters |
| MMS | none | none | characters (`tokens.txt`) |
| Pocket TTS | the model | the model | the model's tokenizer |
| Kokoro | espeak-ng | Vosk dictionary (`common/ru-stress.c`) | IPA from espeak-ng, as kokoro-ru's `ru_g2p.py` (`kokoro/ru-g2p.c`) |
| TeraTTS | numbers spelt out in `teratts/teratts.c` | Vosk dictionary (`common/ru-stress.c`); "+" before a vowel wins | characters with stress marks |
| Vosk | `vosk/vosk.c` (as vosk_tts) | the model's dictionary, g2p for missing words | the model's phonemes, plus BERT tokens |
| RHVoice, espeak-ng | built in | built in | built in |

Planned: Russian text preparation (numbers, abbreviations, Latin words)
as sherpa-onnx FST rules first, then a library of its own for all
modules.

## The layers

**ONNX Runtime and ggml** do the same job: they compute a neural network on a
device (CPU, graphics card). They are used differently:

- ONNX Runtime is a general player. A model comes as a `.onnx` file that holds
  the whole computation graph, and one library runs any such model. Devices
  are *execution providers*; since 1.23 they can be plugins loaded at run time,
  which is how the Vulkan provider (onnx-vulkan-rs) comes in without
  rebuilding ONNX Runtime.
- ggml is a tensor library: the graph of each model is written again in C/C++
  by hand (llama.cpp, whisper.cpp, zonos3.cpp, kokoro.cpp). Every model is a
  port of its own, but the result is usually lighter and faster on Vulkan than
  the general ONNX Runtime.

**sherpa-onnx** is not an engine. It sits between the text and ONNX Runtime:

- it knows the model families: which `.onnx` files a family has, what goes in
  and out of each, and in which order to call them (one call for VITS/Piper;
  acoustic model plus vocoder for Matcha; the denoising loop for Supertonic;
  the language model loop for Pocket TTS);
- the text front end: number and date reading rules (FST), lexicons,
  characters for MMS. Since 2.0 it no longer runs espeak-ng for Piper, Kokoro
  and Kitten: the caller passes the phonemes (`phoneme_codepoints`);
  `sd_sherpa.c` does it for Piper the way piper-phonemize does;
- sentence splitting, speed, speaker id, a callback per finished chunk;
- a C API (and bindings for many languages) and a collection of models
  converted to its layout;
- the choice of the execution provider ("cpu", "cuda", ...; our patch adds
  "vulkan").

The arithmetic itself is done by ONNX Runtime. Our own modules (`sd_kokoro`,
`sd_teratts`, `sd_vosk`) do sherpa's part themselves, in C, for models sherpa
does not know or does worse (the Russian stress dictionaries).

## The Vulkan path

```
sd_sherpa-onnx as sherpa-onnx-vulkan-rs
  -> sherpa-onnx, provider "vulkan"             (packaging/sherpa-onnx/debian/patches)
  -> ONNX Runtime 1.29, private copy            libonnxruntime-sherpa:
                                                  /usr/lib/x86_64-linux-gnu/sherpa-onnx/
  -> libonnxruntime_ep_vulkan.so (onnx-vulkan-rs) onnxruntime-ep-vulkan, same directory
  -> the Vulkan driver (Mesa radv/anv, NVIDIA)
```

- Ubuntu's ONNX Runtime 1.23 cannot load the plugin, and its symbols are
  versioned `VERS_1.23`, so a newer library cannot be dropped in under the
  existing sherpa-onnx: sherpa-onnx is rebuilt against the private 1.29 copy.
  Every module runs in its own process, so the other modules keep using
  Ubuntu's ONNX Runtime.
- The plugin compiles the graph for the device on the first run
  (`VULKAN_EP_COMPILE=1`, set by the patch); without it the card is slower
  than the CPU.
- Inputs of any length run in one session; no padding to fixed lengths is
  needed (checked with a Piper voice).
- The first run on a machine takes about 8 s more: the shaders are compiled
  and kept in Mesa's cache (`~/.cache/mesa_shader_cache`); later starts are
  as fast as on the CPU.
- Only the VITS graphs (Piper, MMS) run. Supertonic uses Pad mode "edge",
  which the plugin accepts but cannot run; the error ends the process
  (sherpa-onnx throws through its C API), so the `-vulkan-rs` synthesizers
  list only Piper and MMS voices.
- Measured with a Piper voice (one phrase, ms; CPU time while on the card):

  | Card | CPU | Vulkan |
  |---|---|---|
  | AMD Vega 8 (integrated) | 245-440 | 240-370, CPU load /40 |
  | NVIDIA GTX 1060 | 138 | 124 |
  | Intel HD 620 (integrated) | ~190, 0.36 s CPU | 450-545, 0.1 s CPU |

  On integrated graphics the card shares the system memory, so it saves no
  RAM; it only takes the load off the CPU.
- Risk: onnx-vulkan-rs has one author, started in October 2026, and is not
  packaged anywhere.
