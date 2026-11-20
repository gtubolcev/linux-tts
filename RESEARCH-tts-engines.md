# Modern TTS engines — research notes (2026-11-08)

Source: Telegram group «Распознавание и синтез речи» (t.me/speech_recognition_ru,
N. Shmyrev / alphacep), messages 2024–2026-11, plus the local system state.

Knowledge sources to follow (field news, new models, the industry in general):
- t.me/speech_recognition_ru — the group above, practical experience, Russian.
- t.me/teraspace_news — news of the field (added by the user 2026-11-14).
Focus: engines usable as a **system** voice (screen reader, notifications, "read selection"):
local, CPU, low latency, stream/interrupt, Russian + English.

## Local system (Xubuntu 26.04)
- speech-dispatcher 0.12.1; modules: espeak, espeak-ng(+mbrola), festival, flite, pico,
  rhvoice, baratinoo/cicero/ivona/kali/voxin (mostly dead or proprietary).
- RHVoice: universe has 1.8.0+dfsg without voices, but **multiverse has 1.14.0-3 with voices**
  (checked 2026-11-10): rhvoice-russian (aleksandr, anna, artemiy, elena, irina, arina,
  evgeniy-rus, mikhail, pavel, tatiana, timofey, umka, victoria, vitaliy(-ng), yuriy, vsevolod,
  aleksandr-hq), rhvoice-english (alan, bdl, clb, slt, evgeniy-eng, lyubov), + uk, pl, cs, …
  and speech-dispatcher-rhvoice 1.14. Voices are non-free (trained nets, no training scripts).
  Nothing to package; the app only needs multiverse enabled.
- Piper wired by hand: `~/.config/speech-dispatcher/modules/piper.conf` → `sd_generic`
  shell pipeline (piper → sox → paplay), binary/models inside the Pied flatpak dir.
  `DefaultModule piper`. Voices: ru_RU-denis-medium, en_US-ryan-high.
- speech-dispatcher ships no Piper config of its own in 0.12.1 (only user-made ones).

## Engines by class

### Classic (formant / unit selection) — fast, tiny, accelerate well
- **espeak-ng** — every language, robotic; also the phonemizer behind Piper.
- **RHVoice** (C++, LGPL-2.1, speechd/SAPI5/NVDA/Android) — the screen-reader choice for
  Russian; blind users run it very fast. Shmyrev (2026-06): "on weak phones even piper
  struggles, only rhvoice". Voices from rhvoice.su/voices; Ubuntu ships them in multiverse
  (rhvoice-russian, rhvoice-english, …), not in universe.

### Light neural, real-time on CPU — the target for a system voice
- **Piper** (VITS, ONNX, rhasspy/piper-voices on HF; now OHF-Voice/piper1-gpl) — the
  de-facto standard; "fine, but sometimes messes up" stress/intonation in Russian.
  Fixes: RuAccent, silero-stress, mitrokun/espeak-ng-data (Russian-improved espeak data).
- **Vosk TTS** — ports checked 2026-11-10: only Python upstream (pip vosk-tts: onnxruntime,
  tokenizers; BERT for prosody in the multi models, 0.9/0.10-multi load ~70 s, issue #46);
  andreytkachenko/vosk-tts-rs (Rust, ort, Apache-2.0, 0 stars, written in 3 days 2026-04,
  then abandoned); browser ports vlapky/vosk-tts-web, FedorMilovanov/gb-vosk-tts (JS,
  onnxruntime-web, so the pipeline is portable). No C/C++ port, no sherpa-onnx support.
  Shmyrev on speechd (vosk-tts #31, 2025-01): pipeline via echo is too slow, needs a native
  module like speechd pico.c, "when we stabilise the algorithm".
  (alphacep, Apache-2.0; 0.8 based on Matcha, 0.10-multi) — Shmyrev's answer
  to "best Russian TTS with low latency and streaming" (2026-05); good stress and
  intonation. Demos: mitrokun.github.io/voskvoice. Some report 0.8–0.9 s per sentence on
  CPU for the multi model.
- **TeraTTS v2** — Russian, "good for CPU" (Den4ik, 2026-08); Android TTS ports exist
  (davnozdu/supertonic-android, teratts-android).
- **Supertonic 3** (~385 MB) — fast multilingual; voices mixable; some artifacts.
- **Pocket TTS** (Kyutai, ~100M) — faster than real time on CPU, voice cloning from a short
  sample; **xVibePocketTTS** (GenVoice, 2026-09) = Russian fine-tune with RuAccent, streaming.
  Shmyrev: of the small ones only Pocket is really real-time.
- **StyleTTS 2** (github.com/yl4579/StyleTTS2, 2023, ~6.4k stars, last push 2024-08;
  added 2026-11-14 by the user) — the architecture and training recipe that Kokoro and
  KittenTTS are built on: style diffusion + adversarial training with WavLM, duration and
  F0 predictors, iSTFTNet/HiFi-GAN decoder; PL-BERT text encoder (English; a multilingual
  PL-BERT for 14 languages exists). Code MIT, but the phonemizer it uses at inference is
  GPL (espeak-ng / phonemizer). Pretrained LJSpeech (1 voice) and LibriTTS (multi-speaker,
  zero-shot cloning) — English only, with a condition: synthesized speech of voices without
  the speaker's permission must be announced as synthetic. Not something to ship as is;
  useful as the recipe for training our own light voices (uk, be, ru) — Kokoro-ru was made
  this way. Runtime for us: export to the Kokoro graph, run by sd_kokoro.
- **Kokoro 82M** — great English quality; Russian only experimental (kokoro-ruslan).
- **Kokoro, Russian** (checked 2026-11-11): huggingface.co/zaakirio/kokoro-ru — Kokoro-82M
  trained for Russian (StyleTTS2 / kikiri-tts recipe), voices sveta + masha (one checkpoint)
  and dima (separate checkpoint), G2P with RUAccent (Python), text input; weights OpenRAIL,
  code Apache-2.0. ONNX exports: huggingface.co/da1353/kokoro-ru (Node G2P on espeak stress,
  worse). Not checked: whether its weights fit the stock Kokoro graph (then kokoro.cpp could
  run them), quality by ear.
  ggml runtimes: **kokoro.cpp** (simonfxr; GGUF huggingface.co/simonfxr/kokoro.cpp-GGUF,
  F16/Q8_0, 54 stock voices, CPU/Vulkan/CUDA/Metal, `kokoro-cli`) — the Vulkan candidate;
  TTS.cpp (mmwillet and forks) — proof of concept, patched ggml, macOS only tested.
  Same name, ONNX Runtime inside: koth/kokoro.cpp, rzafiamy (kokoro-server).
- **KittenTTS** — tiny, English. 0.8 (2026-11-12): nano 15M / micro 40M / mini 80M, ONNX,
  StyleTTS 2 + espeak-ng phonemes, 8 voices, weights Apache-2.0 — a candidate for weak
  machines. KittenTTS 2 (1.7B speech LM, 20 languages incl. ru, voice cloning) is under the
  Stellon Labs Community License (not free, revocable) and needs ~6 GB RAM — not a system
  voice.
- **Silero v5.5** (ru) — good, but restrictive licence (avoided in the group); RuVoice TTS
  (Android) builds on it.
  **Checked 2026-11-12** (files in /home/grigory/Storage/Claude/silero, run only in docker
  `silero-test`, the .pt files are torch.package = pickled code):
  - Licences: all models CC BY-NC-SA 4.0 (README badge says BY-NC, the LICENSE file is
    BY-NC-SA), except `v5_cis_base` / `v5_cis_base_nostress` (MIT, LICENSE_CIS; 29 `ru_*`
    voices, need `+` stress marks) and silero-stress (MIT, stress + homographs + ё).
    → PPA: only cis_base + silero-stress; v5_ru via an installer (speech-settings PLAN).
  - Speed, 4 CPU threads, 24 kHz: v5_6_ru RTF ≈ 0.10, 0.25 s for a short phrase after
    warm-up (first call 1.7 s), load 3.6 s; cis_base + silero-stress RTF ≈ 0.10, stress
    5–30 ms per sentence, homograph зам+ок / з+амке right. Docker image torch-cpu 1.6 GB.
  - Ports: official = PyTorch only. **some1-in-here/silero-tts-onnx** (CC BY-NC-SA,
    "heavily built with AI"): weights moved into reimplemented modules, exported to ONNX
    (acoustic, duration/pitch, vocoder, ISTFT + stress/ngram), torch-free Python frontend
    (numbers, abbreviations, stress, ё, homographs), runtime onnxruntime + numpy + soundfile
    + num2words (all in resolute), ~25× realtime; v5_ru and v5_6_ru only (not cis_base).
    No ggml / Vulkan port (audio.cpp has Silero VAD only); a ggml port would be own work.
- Benchmark: heyneo.com/blog/kokoro-supertonic-inflect-nano-pocket-tts-cpu-benchmark.

- **ZONOS3-Mini** (Zyphra, added 2026-11-10; github.com/Zyphra/ZONOS3-Mini, code MIT, model
  Apache-2.0 on HF Zyphra/ZONOS3-Mini, ~43 languages incl. ru/uk/en; repo from 2026-10-29).
  11-layer LM (gated DeltaNet + global attention, 64-expert MoE), 10 codebooks at 15 Hz,
  iSTFT decoder → 44.1 kHz. **Native C++/ggml port `servers/ggml` (zonos3.cpp)**: GGUF,
  CPU/CUDA/Vulkan/Metal, no Python at runtime; CLI + HTTP server (own v2 API with streaming
  float32 PCM, OpenAI-compatible /v1/audio/speech). Default backbone zonos3-ptq.gguf 0.66 GB
  (whole gguf/ folder 9.3 GB). Voice cloning from ≤ 60 s of audio → 192-dim speaker
  embedding (reusable without the encoder). To check: CPU real-time factor on the Ryzen 5
  3500U, Russian quality, whether a resident server + thin speechd module gives low latency.
  Packaging: own ggml submodule (not sherpa-onnx); a .deb of the CLI/server + a model .deb.
  **Measured 2026-11-10** (Ryzen 5 3500U, static bundled ggml, build in
  /home/grigory/Storage/Claude/zonos3-build, models 1.16 GB: ptq + zac-keep2 + spk-encoder):
  LM RTF 0.22–0.29, but the zac decoder is the bottleneck: RTF 0.99 with its default 4 threads,
  0.70 with `ZAC_THREADS=6` (8 no better), 0.62 with OpenBLAS (`-DGGML_BLAS=ON`, small gain).
  Total ≈ RTF 0.85 + ~2–3 s model load per process → only usable as a resident server.
  Without a speaker the voice is random per seed and often very quiet (en rms 0.006):
  pass `--loudness -20` (−16 clips) or a speaker embedding (`assets/default_voices/*.embedding.npy`;
  no Russian default voice). One ru run with the AmericanMale embedding came out half as long
  (text dropped?). Language is taken from the text, no flag. Samples: _tmp/zonos3/out/.
  User's listening (2026-11-10): only the runs without a speaker + `--loudness` sound right
  (blas-ru, blas-en); English default voices on Russian text glitch or have a heavy accent.
  → For Russian a Russian speaker embedding is needed: e.g. encode a good random-seed output
  with `spk-encoder-cli` to pin that voice, or a freely licensed Russian recording.
  **Done:** seeds 1, 7, 11, 23 (`--loudness -20`) → `spk-encoder-cli` embeddings
  (_tmp/zonos3/out/voices/seedN.embedding.npy); the voice holds on a new text (seed 99).
  User: all four good, seed 1 the best, 7 a bit plainer. Seed 4 broke (text cut, then 12 s).
  Needs ffmpeg at runtime for audio input (`--clone`, spk-encoder-cli).
  **speechd module `sd_zonos3` (2026-11-11, C + GLib, MIT; source _tmp/sd-zonos3, installed in
  the user's ~/.local/libexec/speech-dispatcher-modules + modules/zonos3.conf):** starts
  zonos3-server on a free localhost port on the first message (POST /v2/generate,
  `speaker_mode: embedding`), splits the text into sentences, and sends each one to speechd as soon as
  it is ready (speechd queues module audio without waiting for playback, so the next
  sentence is synthesized while the previous one plays). The first piece is cut at a comma after 40 chars
  (`Zonos3FirstChunk`). Voices are `zonos3-<lang>-<name>` from `*.embedding.npy`. Rate maps to
  `speaking_rate` in UTF-8 bytes/s (`14 · bytes/char · 2^(rate/100)`).
  The server survives module restarts: speechd restarts all modules on every SIGHUP, which the settings
  app sends on each change, and the model reloaded each time (26 s → first sound). Now
  `$XDG_RUNTIME_DIR/sd_zonos3.state` (pid port threads owner released + model) lets the new
  module adopt it; a quitting module leaves a reaper that stops the server after
  `Zonos3IdleTimeout` (600 s) unless adopted. After HUP: 7.5 s instead of 26 s for a short
  phrase incl. playback. Slowness on the loaded laptop: synthesis RTF ≈ 1.2–2 (see the table
  below), so long first sentences wait; streaming within a sentence is the next step.

### ONNX or ggml (2026-11-14)
N. Shmyrev (privately, to the user): ONNX gives nothing, write everything in the ".cpp"
way, on ggml. Decision: ONNX modules stay, onnx-vulkan-rs frozen, the GPU path goes
through ggml with a pilot on kokoro.cpp (xfce4-speech-settings PLAN.md). Facts: Ubuntu 26.04
has libggml0 0.9.11 with separate backend packages (vulkan, hip, blas); ggml Vulkan
already worked here for ZONOS3 (below). Which ggml TTS projects Shmyrev means: asked,
waiting.

### Heavy (GPU, audiobooks / dubbing) — not for a system voice, maybe "read a file" later
(User, 2026-11-12: an audiobook GUI is a separate, far-future project with its own GUI,
heavy models, LLM preprocessing of the text, etc. — not part of speech-settings.)
(User, 2026-11-14: heavy voice LLM / omni models are for people with an RTX 4090 and the
like; they need `sd_` modules too to work under Linux, but that work is outside this
project. Maybe later, as a trial, one module for one large model that prepares the text
itself: homographs, emotions, voice cloning and so on. Plan item: xfce4-speech-settings
PLAN.md, "Later".)
F5-TTS (Russian fine-tune "Misha"; short context), OmniVoice (k2-fsa; good cloning,
stress issues), Qwen3-TTS, CosyVoice3, Higgs v3, FireRedTTS, MOSS, VibeVoice.

- **DMOSpeech 2** (github.com/yl4579/DMOSpeech2, paper arXiv 2507.14988; by the StyleTTS
  author; seen in t.me/teraspace_news, 2026-11-14) — F5-TTS distilled with metric
  optimization: about 2× faster than F5-TTS, better WER and stability; zero-shot cloning.
  Still a diffusion/flow model on a GPU, heavy class; check the licence and Russian.

- **Haru-4B** (huggingface.co/higurashi-ai/Haru-4B, code github.com/higurashi-ai/Haru;
  added 2026-11-13 by the user) — strictly not a TTS: a full-duplex speech-to-speech
  *dialogue* model (listens and speaks at once, 133 ms frames at 7.5 Hz). Qwen3.5-4B text
  backbone + VibeVoice causal acoustic codec (24 kHz, continuous 64-dim latents) + a 4-layer
  MLP MeanFlow head, one sampling step; character set by a text card, voice by a 3 s–3 min
  reference clip. Languages en, es, pt, de, fr, zh, ja, **ru**, ko. Apache-2.0 weights
  (5.3 B params, 10.5 GB bf16). Needs an NVIDIA GPU with ~12 GB (PyTorch), MLX on macOS,
  vLLM-Omni plugin; pretrained base "for research", no instruction following. Not usable as a
  speech-dispatcher module (it answers, it does not read text). Interesting for the far-future
  voice-assistant / "talk to the desktop" idea, not for speech-settings; a quantised CPU or
  Vulkan port (ggml) would be needed for laptops like the user's.

- **ebooker** (romadinov.github.io/ebooker, github.com/romadinov/ebooker; added 2026-11-10)
  — EPUB → M4B audiobook pipeline (Python/uv, Apple Silicon or NVIDIA), not an engine but
  a good field report on Russian TTS. Every chunk is re-transcribed with Whisper and checked
  (numbers, content words, word count, silence/clipping), bad ones re-generated. Measured
  backends: Silero v5 (ru, RTF 0.008), Kokoro-82M (en), ESpeech-TTS-1 (ru, F5, cloning),
  Chatterbox, and **Shtorm-PocketTTS-RU** (CC-BY-4.0, 100M Pocket TTS fine-tune, RTF 0.19 on
  CPU, cloning, obeys acute stress marks; their pick for Russian narration) — a candidate for
  the light class above too. Front-end ideas worth borrowing: explicit stress with RUAccent
  (or silero-stress, which also restores ё), user-editable stress_overrides.txt and
  pronunciation.txt, Latin words in Russian text transliterated via CMUdict, Roman numerals
  → ordinals in the right case, all-caps abbreviations spelt out; measured pause lengths per
  punctuation mark (comma ≈ 0, "…" ≈ 0.45 s, ";" ≈ 0.55 s, "." ≈ 0.7 s).

### audio.cpp (2026-11-15)
github.com/0xShug0/audio.cpp (ShugoAI LLC, Apache-2.0, v0.9.5 of 2026-11-14): one C++
inference engine on ggml for many audio models, with a C API (`-DAUDIOCPP_BUILD_C_API=ON`
→ libaudiocpp.so, include/audiocpp.h: opaque handles, ABI version, sessions, streaming).
Backends CPU / Vulkan / CUDA (upstream also ships prebuilt Ubuntu lib archives; we build
from source). Vendors external/{ggml, sentencepiece, cJSON, cpp-httplib, libyaml,
llama_tokenizer, lz4, utf8proc} — fine for our PPA in a private dir (user: own versions
are OK if they never break distro packages).
Models with free licences that matter to us:
- TTS: kokoro (Apache-2.0), qwen3_tts (ru, Apache-2.0), voxcpm2 (ru, Apache-2.0),
  cosyvoice3 (ru, Apache-2.0), confucius4_tts (ru, Apache-2.0), pocket_tts (CC-BY-4.0),
  supertonic (OpenRAIL-M);
- ASR: gigaam_asr (ru, MIT); VAD: silero_vad (MIT).
Non-commercial, not for the PPA: omnivoice, outetts, xtts_v2, index_tts2, kitten_tts2.
Why interesting: answers the "every model needs its own ggml port" cost of the ggml path
(above) — many ports behind one API, so one speechd module could serve several families.
Also has `piper_tts` (native ggml VITS, espeak-ng phonemes, no onnxruntime; upstream
ships only en_US-lessac, other voices convert from the official .onnx with
tests/piper_tts/convert_piper_onnx.py + audiocpp_gguf — ru_RU-irina-medium converted
fine) and `chatterbox` / `chatterbox_turbo` (MIT).

Measured 2026-11-15 on mikhail (i5-8600K, 6 threads; GTX 1060 6 GB via Vulkan, no
fp16/matrix cores), v0.9.5, q8_0 unless noted, same Russian sentence, two runs each
(spread <0.3 s). Wall time includes model load; RTF = wall / audio length.

| Model | Backend | Wall | Audio | RTF | Peak RSS |
|---|---|---|---|---|---|
| kokoro-82M (en) | CPU | 4.1 s | 8.3 s | 0.50 | 1.5 GB |
| kokoro-82M (en) | Vulkan | 5.7 s | 8.3 s | 0.69 | 0.5 GB |
| qwen3_tts 1.7B CustomVoice (ru) | CPU | 14.2 s | 6.3 s | 2.25 | 3.8 GB |
| qwen3_tts 1.7B CustomVoice (ru) | Vulkan | 11.6 s | 9.5 s | 1.22 | 1.6 GB |
| piper ru irina medium (orig) | CPU | 0.82 s | 8.1 s | ~0.1 | 480 MB |
| piper ru irina medium (orig) | Vulkan | 0.34 s (3.1 s 1st run: shader cache) | 8.1 s | ~0.04 | 250 MB |
| piper ru irina via piper-tts/ORT (reference) | CPU | 0.8 s (synth alone 0.21 s) | 7.3 s | 0.03 synth | 245 MB |
| cosyvoice3 0.5B (clone of the irina sample) | CPU | 32 s | 5.5 s | 5.8 | 2.8 GB |
| cosyvoice3 0.5B | Vulkan | 14.7 s | 7.8 s | 1.9 | 1.4 GB |
| omnivoice (voice design) | CPU | 41.7 s | 7.0 s | 5.9 | 1.8 GB |
| omnivoice | Vulkan | 15.9 s | **0 s — empty WAV, no error** (upstream bug to report) | — | 1.0 GB |

Notes:
- Small models (kokoro) gain nothing from Vulkan on a 1060; the large ones gain ~2×
  but stay above real time. Piper on Vulkan is the one real win: same voices, same
  espeak-ng phonemes, no onnxruntime. On CPU Piper stays on ONNX (sherpa-onnx), as decided.
- qwen3 on Vulkan produced 9.5 s of audio vs 6.3 s on CPU for the same text and seed —
  check for trailing silence / hallucination by ear.
- Upstream piper_tts doc: on Mali GPUs (RK3588) Vulkan duration flows are
  non-deterministic and can abort with "non-finite duration". Mali is phones/SBCs, not
  our target; there a fallback to ONNX / onnx-vulkan-rs can come later (user, 2026-11-15).
- omnivoice: 600+ languages (incl. be, kk, ky, tt, ba, cv, sah, uz, tg, hy, ka, os, ab),
  but weights are CC-BY-NC (Emilia data) — NC download only. `--instruct` accepts only a
  fixed vocabulary (male/female, child/middle-aged/elderly, low/moderate/high pitch).
- cosyvoice3: Apache-2.0, ru, but clone-only: needs a reference WAV + transcript (a
  sample is needed; the test used the irina Piper sample, whose dataset licence is
  "Unknown" — use a clearly licensed reference next time). Too slow for screen reading.
- outetts (Llama-OuteTTS-1.0-1B, 23 langs incl. ru/uk/be/ka/lt/lv): CC-BY-NC-SA; the q8
  GGUF also needs a DAC companion that only the deprecated installer converts (PyTorch).
  Not run.

Verdict: a speechd module on audio.cpp could cover (1) Piper on GPU and (2) a heavy tier
(qwen3_tts, cosyvoice3, chatterbox) for GPUs above a 1060.

### kokoro-ruslan (checked 2026-11-15)
github.com/igorshmukler/kokoro-ruslan: a Russian training pipeline (PyTorch, MPS-first)
on the RUSLAN single-speaker corpus. Despite the name it is not Kokoro-82M (StyleTTS2 +
iSTFTNet) but its own FastSpeech2-like transformer (mel, duration, pitch, energy, stop
token) with a HiFi-GAN vocoder, so kokoro runtimes (kokoro.cpp, audio.cpp kokoro_tts,
sherpa-onnx) cannot load it. No released weights (empty models/ and vocoder_models/, none
on HF), no LICENSE file, training still unstable per its release notes (v0.0.35,
2026-04). The RUSLAN corpus licence must be checked before any use. Verdict: not usable
now; watch for released weights.

### Other ggml TTS ports (checked 2026-11-15)
- PABannier/bark.cpp (MIT): Suno Bark on ggml; last commit 2024-11. Bark is slow and
  hallucinates — not a system voice.
- maxilevi/vits.cpp (GPL-3): undergraduate ggml port of VITS, last commit 2024-08. The
  author's write-up (maxilevi.com/blog/vits) finds the HiFi-GAN decoder convolutions
  dominate CPU time and fixes them with im2col; no end-to-end RTF, no comparison with ORT.
  Superseded by audio.cpp piper_tts.
- seanghay/vits.cpp: not ggml — a tiny VITS example on ONNX Runtime, no licence,
  abandoned 2023. Nothing sherpa-onnx doesn't do.
- NVIDIA ACE Chatterbox (NVIGI plugin): Chatterbox Turbo (en) / Multilingual (23 langs
  incl. ru, ≥3.5 GB VRAM) on ggml with CUDA/Vulkan/D3D12, but a Windows game SDK. Use
  Chatterbox through audio.cpp or CrispASR instead.
- Misha24-10/F5-TTS_RUSSIAN: F5-TTS v1 fine-tunes for Russian (an accent_tune variant
  for stress marks), CC-BY-NC-4.0, clone-only.

### CrispASR (2026-11-15)
github.com/CrispStrobe/CrispASR (MIT, very active): whisper.cpp fork grown into one ggml
binary with 145 backends (69 TTS), CLI + HTTP server + C-ABI. Relevant:
- piper with a built-in Russian G2P: an 812,953-word IPA dictionary with resolved stress
  (bene-ges/ru_g2p_ipa_bert_large, CC-BY-4.0, cstr/g2p-dicts `ru_g2p_ipa.tsv`) + rules for
  palatalisation, voicing assimilation, final devoicing and vowel reduction. Honours `ё`
  and U+0301 stress marks. 17,359 heteronyms (за́мок/замо́к, все/всё) are deliberately left
  out of the dictionary — they need a sentence-aware model. Useful for our "Russian text
  preparation" item.
- chatterbox (multilingual V3 T3, 23 langs, MIT), cosyvoice3, qwen3-tts, voxcpm2 (30 langs,
  Apache-2.0), supertonic-3 (31 langs, OpenRAIL-M), f5-tts (en/zh tokeniser; a Russian
  fine-tune would need conversion).
- Prebuilt Vulkan tarballs exist; we build from source.

Build notes (commit 9ddae4590, ubuntu:26.04 docker on mikhail): `-DGGML_VULKAN=ON
-DGGML_BACKEND_DL=ON -DBUILD_SHARED_LIBS=ON -DGGML_NATIVE=OFF -DGGML_CPU_ALL_VARIANTS=ON`
(BACKEND_DL refuses GGML_NATIVE); the full `all` target fails linking some tests, so build
only `--target crispasr-cli`. Piper voices convert with models/convert-piper-to-gguf.py
(numpy + onnx only; ru irina → 30 MB f16). The F5 converter needs torch (CPU wheel);
the Misha24-10 Russian F5 vocab has the same 2545 entries as upstream, so it converts.
Cloning backends refuse to run without `--i-have-rights` (consent attestation).

Measured 2026-11-15, same machine and sentence as the audio.cpp table:

| Model | Backend | Wall | Audio | RTF | Peak RSS |
|---|---|---|---|---|---|
| piper ru irina (dict G2P) | CPU | 5.1 s | 7.6 s | 0.66 | 360 MB |
| piper ru irina (dict G2P) | Vulkan | 5.0 s | 7.6 s | 0.66 | 360 MB |
| chatterbox multilingual V3 q8 (ru, built-in voice) | CPU | 22 s (111 s 1st run incl. download) | 7.3 s | 3.0 | 1.4 GB |
| chatterbox multilingual V3 q8 | Vulkan | 9.4 s (12.8 s 1st) | 7.2 s | 1.3 | 0.8 GB |

Piper's 5 s are dominated by start-up (most likely loading the 813k-entry dictionary,
paid once in a resident speechd module); the per-utterance synthesis is not printed.
F5-ru (Misha24-10 F5TTS_v1_Base_v2, q8_0, 32 ODE steps, cfg 2) with a CC0 reference:
Common Voice 21 ru clip common_voice_ru_27302223 (7.0 s, from the CC0 mirror
Sh1man/common_voice_21_ru), "Болгария также с глубоким удовлетворением отмечает
проведение совещания высокого уровня по ядерной безопасности."
- CPU: 761 s wall for 5.1 s of audio (RTF ≈ 150), 1.1 GB RSS, only ~2.5 cores busy; the
  text was split into two chunks, each re-running the 7 s reference. Even Vocos takes
  3.5 s for 3 s of audio: the port has hand-written CPU convolutions, and the graph
  compute ignores `n_threads`.
- Vulkan: hung at 0 % GPU after the first chunk started (killed after 25 min).
- Verdict: CrispASR's F5 port is not usable yet (PyTorch F5 on CPU is roughly RTF 1–3).
  Listening file: bench/cpu-f5-ru-cv-1.wav.
Stress comparison by ear (to do): bench/stress-crisp-dict.wav (dictionary G2P) vs
bench/stress-audiocpp-espeak.wav (espeak-ng), text "Мы пили молоко и ели творог. Все
пришли, всё готово. В две тысячи двадцать шестом году."

### BlueTTS / BlueV3 (checked 2026-11-18)

github.com/maxmelichov/BlueTTS (code MIT), weights notmax123/BlueV3(-onnx), v3.0.1, bundle
updated 2026-11-18. Flow-matching text-to-latent + codec decoder, 8 Euler steps, plain ONNX
Runtime on CPU (also CUDA/OpenVINO/TensorRT, WebGPU); RTF 14–18× real time on a Ryzen 9 7950X.
Files: text encoder 28 MB + flow 260 MB + decoder 101 MB (+ reference encoder 136 MB for
cloning from ~10 s). 15 LibriTTS-R voices (CC BY 4.0). Own Rust G2P `polyg2p` (Apache-2.0,
Russian stress/ё from RUAccent dictionaries, MIT).
- **Russian is a trained language** (he, en, de, es, it, ru, yi): README/RELEASE_NOTES of the
  bundle, `lang="ru"`. (Corrected the same day: FRONTEND.md has a stale "not part of this
  release" line, and CHECKS.md only smoke-tests ru.) v3.0.1 per-language check, 13 voices ×
  4 sentences: Russian UTMOS 3.76 (en 4.43, de 3.95), speaker similarity 0.40 — all voices are
  English LibriTTS-R speakers, so Russian is spoken by "foreign" voices; listen for accent.
  Russian G2P: polyg2p with RUAccent stress/ё dictionaries. "Numbers, dates, prices are spoken
  as words automatically" — whether that covers Russian (and how well) is not verified.
- **Decoder is a fine-tune of the Supertonic-3 vocoder under BigScience OpenRAIL-M**
  (use-based restrictions) → not a free licence, not for our PPA, like Supertonic itself.
- Text normalisation is Hebrew only (heb-tts-normalizer); nothing for Russian.
Test 2026-11-18 on mikhail (docker, --cpus=3, i5-8600K, BlueTTS cbe5150), 10 Russian phrases
× maya/adam/david × 8/4 steps; WAVs in `_tmp/bluev3/` (README.txt there):
- Speed: RTF 0.19 at 8 steps, 0.12 at 4 steps (≈5–8× real time on 3 cores), RSS ≈1.2 GB,
  load 1.3–2.4 s per voice. Needs `ORT_NUM_THREADS` = real cores: by default it takes
  min(8, cpu_count) threads and under a 3-CPU limit RTF falls to ≈1.0.
- Built-in normalizer for ru is weak: no case agreement ("нет пять рублей", "с девять до
  восемнадцать", "две тысячи двадцать шесть года"), "15%" → "пятнадцать percent", "руб."
  left, Roman numerals and decimals ("XIX", "2,5") untouched, phone in 【…】 with "plus".
  For a module: our normalizer first (ru-normalizr), then `normalize_text=False`.
- Listening (user, 2026-11-18): voices are decent; homographs fail ("зАмок на двери старого
  зАмка"); "Ты придёшь завтра?" has no question intonation (flat), although "?" survives into
  the phoneme string; "нет пять рублей" (normalizer, see above).
- Stress hints don't work: polyg2p ignores U+0301 (за́мок/замо́к both → zˈamək) and reads "+" as
  "плюс". It also turned "все" into "всё" (fsʲˈɵ) out of context. Only way to control stress:
  our own stress/G2P → IPA and `synthesize(..., text_is_phonemes=True)`.
Licence still blocks the PPA (decoder OpenRAIL-M); at most a user-downloaded module.

### Supertonic upstream and its forks (checked 2026-11-20)
Repo supertone-oss-archive/supertonic (sample code MIT, model OpenRAIL-M, so not in the PPA).
- **Archived 2026-09-09.** The official Voice Builder closed on 2026-08-31, so there is no
  official way to make new voices. Community replacements:
  - kdrkdrkdr/supertonic.embed: fits `style_ttl` (50×256) and `style_dp` (8×16) to a WAV
    through WavLM, needs a CUDA GPU, licence not stated;
  - saurabhv749/supertonic3-voice-clone: MIT, ~2.6 GB of GPU memory, 6–30 min per voice;
  - Fawzan09 voice builder: a Colab notebook, no licence.

  All of them output the same style JSON as F1…M5, so a voice import works for these files too.
  Cloning only with the speaker's consent (see tts-direction).
- **Reported WER:** ru 3.99, uk 1.23.
- **Front end:** NFKD, text wrapped as `<lang>…</lang>`; `na` means no language. sherpa does
  all of this already. Chunks are at most 300 characters (120 for ko/ja), with 0.3 s of silence
  between them; sherpa's `silence_duration` matches.
- **Expression tags** such as `<laugh>`, `<breath>` and `<sigh>` (10 in all). Untested;
  normalizers must not strip them.
- **U+0301 has an index (146) in unicode_indexer.bin.** Stress marks might work; we strip
  them now and send only ё. To test, writing to files only.
- **Speed:** sherpa and the sample code default to 1.05, and sd_sherpa multiplies the user's
  rate by 1.05. The MentalGear fork measured 1.0 as more natural and with fewer clipped
  words, and wrote an upstream PR (docs/upstream_pr_speed_default.md).
- **Seed:** the starting noise has no seed (`seed=-1` in sherpa), so the same phrase can come
  out clean once and "hiccupy" the next time; the vocoder noise moves word timing more than
  the style does (MentalGear docs/GLITCH_MITIGATION.md). A fixed seed makes renders
  repeatable and lets a test set compare like with like. Try a fixed seed in sd_sherpa.
- Ports: Supertonic MNN (fp16/int8 builds, CLI vra/supertonic-mnn, no licence);
  Topping1/Supertonic-Voice-Mixer (MIT, mixes styles: the same as our planned voice mixing).

Forks (1604 in all; the ones with real commits ahead of upstream):

| Fork | Ahead | Of interest |
|---|---|---|
| MentalGear/supertonic | 81 | emotion and style steering: `s' = normalize(s + α·Δ)` on style_ttl, with no ONNX change (angry and surprised from RAVDESS, which is NC, a one-speaker proof); speed 1.0; glitch and seed study, listening benches |
| DevGitPit | 104 | Android text normalizer: em dash → comma, ranges, currency, title case lowered at chunk boundaries, trailing comma → period, regex lexicon |
| groxaxo/supertonic-express | 52 | MIT. CPU timestep reuse, OpenVINO, smart chunking, 15 steps by default, CPU benchmark checked by ASR |
| wwongw21 | 5 | trims emitted audio chunks, bounds Unicode input |
| calebrio02 | 7 | Docker, OpenAI-compatible API |
| nero-dv | 3 | GPU |

## Runtimes worth building on
- **sherpa-onnx** (k2-fsa, Apache-2.0, C/C++ API, CPU) — one runtime for many TTS model
  families (VITS/Piper, Matcha, Kokoro, Kitten, …) and for STT (Vosk, GigaAM v3, Whisper,
  Parakeet). To check: exact list of TTS families in the current release, whether Vosk TTS
  and Supertonic/Pocket load there.
- **sherpa-onnx ≥ 2.0.0 (#4055) dropped espeak-ng / piper-phonemize** (GPL-free now).
  TTS front ends: built-in lexicon, or external phonemes passed through
  `SherpaOnnxGenerationConfig` (`phoneme_codepoints` / `tokens`; `data_dir` deprecated).
  → `sd_sherpa` phonemizes Piper/VITS/Kokoro input itself with the system libespeak-ng
  (GPL-3 module, which is fine for speech-dispatcher).
- Packaging: nobody ships sherpa-onnx for Debian/Ubuntu; resolute has
  libonnxruntime-dev 1.23.2, so only sherpa-onnx itself is missing (own PPA, 2.0.1+ds with
  vendored small deps: kaldi-native-fbank(+kissfft), kaldi-decoder, simple-sentencepiece,
  openfst fork, hclust-cpp, eigen 5.0.1, json, utf8proc).
- **Speed check 2026-11-10** (Ryzen 5 3500U, ru_RU-dmitri-medium, 6 s of speech, whole process
  incl. model load): piper 1.2 (Pied, bundled onnxruntime 1.14) 1.15 s; piper 1.8 + Ubuntu
  onnxruntime 1.23.2 2.35 s; piper 1.8 + upstream onnxruntime 1.23.2 2.1 s (Ubuntu's build only
  ~12 % slower). piper 1.8 hard-codes `SetIntraOpNumThreads(1)`; local patch (branch `threads`
  in _tmp/piper1/src, env PIPER_NUM_THREADS, default 0 = per core) → 1.85 s. The rest of the gap
  to 1.2 is still open (to check: ORT_ENABLE_BASIC vs ALL, disabled mem arena, load vs
  synthesis time, ort 1.14 vs 1.23). sherpa-onnx CLI also defaults to 1 thread: MMS ru RTF
  1.16 → 0.49 with 4 threads; Supertonic 3 ru RTF 0.70 (8 threads), Pocket en 0.97 (1 thread).
  **Decision (user, 2026-11-10): build against Ubuntu's libonnxruntime1.23, no upstream
  onnxruntime copy.** *(Revised 2026-11-15: "Мы можем собирать самую свежую версию, просто ставить её
  независимо" — an own newest onnxruntime in a private dir is allowed.)* (Upstream could only live in a private dir with RPATH: same file name
  libonnxruntime.so.1.23.2, but SONAME .so.1 vs .so.1.23 and no VERS_1.23 symbol versions.)
- (2026-11-11) Interim speechd modules through sd_generic + sherpa-say (load per phrase,
  ~6–7 s per short phrase): `supertonic-generic` (31 languages × 10 speakers,
  supertonic-LANG-N) and `pocket-generic` (pocket-bria, pocket-loona) in the user's
  ~/.config/speech-dispatcher/modules (sources _tmp/sherpa/speechd). **User's verdict:
  Supertonic Russian is decent but ZONOS3 Russian is better; Supertonic English is good.** supertonic-ru-7 (sid 6): "very decent, on the ZONOS level" (preliminary).
- Russian STT for the later "Speech input" tab: GigaAM v3 (sherpa-onnx, RTF ~0.05–0.1 on
  Ryzen, INT8), Vosk, Whisper, Parakeet.

## Consequences for the settings app
1. speech-dispatcher stays the bus (Orca, Firefox, Chromium, xdg speech portal speak to it);
   the app configures it and does not replace it.
2. Its module list is outdated: modern engines come only through `sd_generic` shell
   pipelines (fragile: no streaming, no index marks, process per message, quoting bugs).
   Better: **one native module `sd_sherpa`** (C, libspeechd-module + sherpa-onnx C API) that
   loads ONNX voices of several families → Piper, Matcha/Vosk, Kokoro, Kitten from one
   place, low latency, interruptible. Possibly upstreamable to speech-dispatcher.
3. Voice catalog in the app: per engine an index (piper-voices voices.json; sherpa-onnx
   tts-models release; rhvoice.su voices), filter by language, sample playback, install to
   `~/.local/share/<app>/voices/`, then (re)generate module config + restart the daemon.
4. Russian stress: optional preprocessing (RuAccent / silero-stress) is a later option.

## STT / dictation landscape (for the later "Speech input" part; from the user, 2026-11-08)
- **IBus-Speech-To-Text** (github.com/Manish7093/IBus-Speech-To-Text) — dictation as an IBus
  input method, offline Vosk, GTK4/libadwaita settings. The closest thing to native
  dictation; works in any toolkit that speaks IBus → the natural integration point for us too.
- **voxtype** (github.com/peteonrails/voxtype) — push-to-talk dictation daemon for Linux.
- **Ubuntu: "Myna"** — Canonical's own effort (per the user; details still to look up).
- GNOME Shell extensions: Speech to Text (OpenRouter, cloud), gnome-speech2text and
  speech2text-extension (Whisper / whisper.cpp, local), OpenWispr (local companion daemon).
  Speed of Sound — standalone GTK app, Whisper.
- WebKitGTK: Web Speech API recognition locally through whisper.cpp + GStreamer.
- GNOME Discourse 2026 proposal "Desktop-wide offline speech-to-text / dictation support":
  D-Bus interfaces + an XDG portal, because Wayland forbids injecting keystrokes into
  other windows. Our STT design should follow that (input method / portal), not xdotool.

## Speed and resources on the user's laptop (2026-11-11)
Ryzen 5 3500U (4 cores / 8 threads), 5.7 GB RAM, load average 2–6 from other apps (Firefox,
Telegram) during the run. One phrase per language (ru ≈ 140 chars, en ≈ 160 chars),
best of 2 runs. Script _tmp/bench/bench.py, RHVoice through _tmp/bench/rhv/rhv-say (C API).
"Call" = one process as speechd's generic module runs it (start + model load + synthesis);
ZONOS3 = warm resident server (the load is paid once). RTF = time / audio length (< 1 is
faster than real time). CPU = CPU-seconds per second of speech.

| Engine | Lang | Audio, s | Call, s | RTF call | RTF synth only | CPU s / s speech | Peak RAM |
|---|---|---|---|---|---|---|---|
| RHVoice (Aleksandr / SLT) | ru | 11.2 | 0.75 | **0.07** | — | 0.06 | 27 MB |
| RHVoice | en | 9.1 | 0.56 | **0.06** | — | 0.06 | 19 MB |
| Piper ru_RU-dmitri-medium | ru | 7.7 | 4.9 | 0.64 | — | 1.9 | 231 MB |
| Piper en_US-ljspeech-high | en | 10.1 | 15.2 | 1.50 | — | 7.7 | 288 MB |
| MMS-TTS (sherpa) | ru | 9.1 | 9.5 | 1.05 | 0.82 | 5.9 | 242 MB |
| Supertonic 3 (sherpa, 8 steps) | ru | 10.8 | 15.0 | 1.40 | 1.25 | 8.3 | 307 MB |
| Supertonic 3 | en | 11.1 | 13.2 | 1.18 | 1.03 | 7.4 | 309 MB |
| Pocket TTS (sherpa) | en | 7.9 | 16.2 | 2.05 | 1.83 | 12.1 | 581 MB |
| ZONOS3-Mini, server, 4 decoder threads | ru | 8.7 | — | — | 1.50 | 5.3 | 1.93 GB |
| ZONOS3-Mini, 4 threads | en | 10.4 | — | — | 1.45 | 5.1 | 1.93 GB |
| ZONOS3-Mini, 8 threads | ru | 8.7 | — | — | 1.57 | 5.6 | 1.94 GB |
| ZONOS3-Mini, 8 threads | en | 10.4 | — | — | 1.51 | 5.4 | 1.94 GB |

ZONOS3 server load: 6 s (10 CPU-s) on a calmer system, 15 s under load.
Conclusions: on this CPU every neural engine is around real time or slower, so the first
sentence always waits; only RHVoice is instant. 4 decoder threads beat 8 (less CPU, a bit
faster) → the user's zonos3.conf now has `Zonos3Threads 4`; the module default should change to
4. sherpa-say uses nproc threads; Supertonic/Pocket probably also oversubscribe (to try: -t 4,
fewer Supertonic steps). The numbers depend on the background load.

### All our modules side by side (2026-11-14, 03:00, laptop idle, load ≈ 0.1)
One Russian sentence (122 chars), one process = model load + synthesis, 4 threads, output
to a file (own test tools: kokoro-test, teratts-test, vosk-test, sherpa-say with OUT=).
RHVoice and Piper from the table above (2026-11-11, laptop under load). Script and output
in XFCE-patches/_tmp/bench2.

| Engine | Audio, s | Wall, s | RTF incl. load | CPU s / s speech | Peak RAM | On disk |
|---|---|---|---|---|---|---|
| RHVoice | 11.2 | 0.75 | 0.07 | 0.06 | 27 MB | ~20 MB / voice |
| Piper ru dmitri medium | 7.7 | 4.9 | 0.64 (loaded system) | 1.9 | 231 MB | 63 MB / voice |
| TeraTTS v2 (sd_teratts) | 7.6 | 3.0 | **0.39** | 1.0 | 820 MB | 371 MB |
| Supertonic 3 (sherpa, 5 steps) | 9.5 | 4.4 | 0.47 | 1.6 | 312 MB | ~300 MB |
| MMS ru (sherpa) | 7.7 | 4.3 | 0.56 | 1.8 | 285 MB | ~150 MB |
| Kokoro ru (sd_kokoro) | 8.0 | 8.0 | 1.00 | 3.0 | 560 MB | 310 MB / model |
| Vosk 0.9, no BERT | 8.8 | 10.7 | 1.21 | 4.1 | 390 MB | 889 MB |
| Vosk 0.9, BERT | 10.4 | 15.2 | 1.47 | 4.5 | 1.28 GB | 889 MB |

Tiers for weak machines: RHVoice (instant, tiny) → Piper / sherpa (MMS, Supertonic; ~0.3 GB,
about twice real time) → TeraTTS (fastest neural, but 0.8 GB RAM) → Kokoro (real time,
0.56 GB) → Vosk (slowest and, with BERT, the biggest). The Silero server (PyTorch) is not
in the table: RTF ≈ 0.10 in docker, its memory not measured yet.

### ZONOS3 on the Vega 8 through ggml Vulkan (2026-11-11, night)
Build: `-DGGML_VULKAN=ON` (docker image zonos-vk = zonos-build + libvulkan-dev glslc
spirv-headers; script _tmp/zonos3/vk/build-vk.sh; binaries
/home/grigory/Storage/Claude/zonos3-build/build-vk/). **Needed a fix:** ggml's Vulkan backend
reports the Vega 8 (uma) as `GGML_BACKEND_DEVICE_TYPE_IGPU`, and zonos3 picks only `..._GPU`,
so `--gpu` silently ran on the CPU. Patch _tmp/zonos3/vk/igpu.patch (zonos3.cpp, zac.cpp:
fall back to IGPU, treat IGPU as GPU) — worth sending upstream to Zyphra.
Same phrases and script as the table above, at a quieter moment (load 2–4):

| ZONOS3 variant | ru RTF | en RTF | CPU s / s speech | RAM (RSS) | VRAM |
|---|---|---|---|---|---|
| CPU, 4 decoder threads | 0.96 | 0.93 | 3.3 | 1.85 GB | — |
| Vulkan, LM on GPU, decoder on CPU (`--zac-cpu`) | 0.92 | 0.89 | 2.2 | 1.29 GB | |
| **Vulkan, all on GPU (`--gpu`)** | **0.55** | **0.52** | **0.27** | **0.32 GB** | ≈1.3 GB |

Server load 3.8 s. VRAM is the 2 GB carve-out reserved by the BIOS, which the CPU can't use anyway,
so the GPU variant also frees ~1.5 GB of system RAM. The first measurement under load
(previous table, RTF 1.5) shows how much the CPU variant suffers from other apps; the GPU one
barely uses the CPU. Output differs slightly (f16 on GPU: ru 8.8 s vs 8.67 s); the user
compared _tmp/bench/out/zonos-{cpu,vulkan}-ru.wav by ear: no audible difference.

**Done (2026-11-11):** zonos3-tts 2~local is built with Vulkan + OpenBLAS + igpu.patch
(build dir build-vkblas, Depends libvulkan1, Recommends mesa-vulkan-drivers). In sd_zonos3,
`Zonos3Gpu 1` is the default: the server starts with `--gpu`, and if it dies while
loading (no memory etc.), it is started again with `--cpu`. The mode is kept in the state
file, so only a server with the same setting is adopted. The default number of threads is now
min(CPUs, 4). zonos3-say tries `--gpu` and retries with `--cpu` (`ZONOS3_BACKEND=cpu` skips
the GPU). Protocol test on the GPU: cold first audio 8.7 s, adopted 2.1 s (CPU: 35.6 / 8.2 s).
A reaper bug was fixed along the way: the forked reaper inherited `server_child`, so
`waitpid` failed for it, it took the server for dead, removed the state and left the
server orphaned. The first live test then showed two more problems:
- `spd-say` without `-w` autospawns speechd, disconnects at once, and speechd quits 5 s later
  (`Timeout 5`) without waiting for a module that is still loading. The message is lost.
- The module swallowed QUIT while it waited for the server and was killed without releasing it.

Now the release-time reaper is replaced by a **watcher forked with the server**: every 5 s
it checks the state file, and when the owner is 0 or no longer a living sd_zonos3
process for Zonos3IdleTimeout seconds, it stops the server. QUIT during a wait ends
the message and the module. Tested: normal and adopted runs, QUIT and SIGKILL while
loading. In every case the server is gone after the timeout. Note for a PPA build: `GGML_NATIVE=ON` ties the binaries to this CPU.

### sherpa-onnx threads and Supertonic steps (2026-11-11, Ryzen 5 3500U, 4C/8T)

Same 12 s phrase, best of 2 (`_tmp/bench/sherpa/run.sh`):

| engine | steps | threads | RTF | wall | CPU s |
|---|---|---|---|---|---|
| Supertonic ru | 8 | 8 | 1.06 | 13.1 s | 77 |
| Supertonic ru | 8 | 4 | 0.92 | 11.3 s | 38 |
| Supertonic ru | 8 | 2 | 0.91 | 11.3 s | 21 |
| Supertonic ru | 5 | 4 | 0.59 | 7.8 s | 25 |
| Supertonic ru | 3 | 4 | 0.42 | 5.9 s | 19 |
| Pocket en | – | 8 | 1.63 | 14.8 s | 85 |
| Pocket en | – | 4 | 1.12 | 10.5 s | 44 |
| Pocket en | – | 2 | 1.15 | 11.3 s | 23 |

8 threads (SMT) is the worst; 4 is optimal, 2 nearly as fast at half the CPU.
User's verdict by ear: 5 steps good, 3 bad. sherpa-say (sherpa-onnx 2.0.1-3~local) now
defaults to 5 steps and min(nproc, 4) threads, `-n STEPS` overrides. Pocket stays slower
than real time even at 4 threads. Supertonic misplaces Russian stress ("к+афе"), see PLAN.

### Vosk TTS through plain ONNX Runtime (checked 2026-11-11)

vosk_tts (Python) is a thin layer over onnxruntime: the model directory has `model.onnx`
(VITS-based acoustic model + vocoder, 22050 Hz), `dictionary` (word, probability, phonemes
with stress; best entry per word kept), `config.json`, optional `bert/model.onnx` +
`bert/vocab.txt` (WordPiece). Pipeline: split text on punctuation/spaces → dictionary lookup,
`g2p.convert(word)` for unknown words → phoneme ids (`^`/`$`, blanks or the 5-field
"multistream" tuples via `phoneme_id_map`) → BERT per-word embeddings (1×768×N, zeros
without BERT) → one `run()` with inputs `input`, `input_lengths`, `scales`
[noise, 1/rate, duration noise], `sid`, `bert`, (`phone_duration_extra` for multistream_v3).
No numpy-only magic, so a C port (sd_vosk or inside sd_sherpa) needs: the dictionary
lookup, a port of g2p.py (Russian rules), a WordPiece tokenizer, the multistream mapping.
Not drop-in for sherpa-onnx (its VITS path has no bert/multistream inputs). The only
existing port is vosk-tts-rs (Rust, `ort`, downloads ONNX Runtime; abandoned).
Bonus: the dictionary carries stress, so known words ("кафе") are stressed right without
RUAccent.

### sd_vosk (2026-11-11)

Native speechd module, C + GLib + json-glib + ONNX Runtime C API (Ubuntu libonnxruntime
1.23), MIT; source `_tmp/sd-vosk` (vosk.c = port of vosk_tts, sd_vosk.c, vosk.conf,
build-deb.sh, proto.py, bench.c; Python counterpart `_tmp/vosk-ref/bench.py`). The port
matches vosk_tts on 5 reference texts (tokens, BERT rows, phoneme ids). Packages:
speech-dispatcher-vosk 0.1-1~local + vosk-tts-model-ru-0.9-multi 0.9-1~local (780 MB,
ships `dictionary.sorted` = best entry per word, mmapped, binary search). Voices
`vosk-ru-female-1..3`, `vosk-ru-male-1..2`; options VoskModelsDir, VoskVoice, VoskBert,
VoskPreload (default on, loads in a thread; STOP works while waiting), VoskThreads,
VoskFirstChunk. A warm-up sentence runs after the load. No AddModule needed: speechd
autodetects modules while speechd.conf has no AddModule line.

**Slow load and the fix.** In the sequential executor ORT's memory planner
(allocation_planner.cc `FindReusableTensor`, walks the whole free list for every output)
is quadratic on the 17 000-node acoustic graph: 37 s at optimization level 0, ~70 s at
ALL; onnxsim (12 900 nodes) 55 s, `.ort` format and pip ORT 1.31 no better (found with
gdb + Ubuntu debuginfod symbols). `ORT_PARALLEL` plans no buffer reuse → 4 s. It needs
inter-op threads > 1, otherwise ORT silently falls back to sequential
(inference_session.cc: no inter-op pool → SEQUENTIAL). BERT stays sequential + ALL.

**C vs Python (vosk_tts, pip ORT 1.31.1, docker), same 5 texts, 4 threads, Ryzen
4-core/8-thread, 5.8 GB RAM with 4 GB in swap and Firefox busy, so absolute RTF is noisy:**

| | load | memory (peak RSS) | synth RTF |
|---|---|---|---|
| sd_vosk / bench.c, BERT | 6–8 s | 1.28 GB | 1.13 / 1.42 (interleaved runs) |
| Python vosk_tts, BERT | 106–148 s | 1.4–1.8 GB | 1.12 / 1.44 (interleaved runs) |
| sd_vosk, no BERT | 3.8 s | 0.33–0.40 GB | ~0.7 (quieter moment) |
| C, all sequential (old) | 70–94 s | 1.27 GB | 0.77 |

Synthesis speed is the same (same ONNX graphs, the time is in the model); the gain is the
load: ~20× faster, plus no Python. Python with ORT's default thread count (all 8) was
slower than with 4. Without BERT the speech is flatter; switch: `VoskBert 0`.

### iGPU fix sent upstream (2026-11-11)

Zyphra/ZONOS3-Mini PR #2 (github.com/Zyphra/ZONOS3-Mini/pull/2, branch gtubolcev:igpu,
clone /home/grigory/Storage/Claude/zonos3-pr): shared `servers/ggml/src/backend-device.h`
(GPU → IGPU → CPU; is_gpu counts both) used by zonos3, zac, zac-encoder, ssl-encoder,
spk-encoder and the ssl-reference-state test. Built on main f9863b3 (needs cargo now:
docker image zonos-vk-blas-rust = zonos-vk-blas + Rust from rust:1-slim), `--gpu` →
Vulkan0 on the Vega 8. Once merged, drop igpu.patch from our zonos3-tts package.

### sd_sherpa (2026-11-11)

Native speechd module, C + GLib, MIT; source `_tmp/sd-sherpa` (sd_sherpa.c, sherpa.conf,
build.sh, build-deb.sh, proto.py = protocol test that writes WAVs to out/). The sherpa-onnx C
API runs inside the module, no server: Supertonic 3 loads in 0.8–1.3 s, so the restart on every
speechd SIGHUP costs about a second, unlike ZONOS (6–15 s). One model is loaded at a time.
Models are found in `/usr/share/sherpa-onnx/tts` and `~/.local/share/sherpa-onnx/tts`:
Supertonic (`supertonic-<lang>-<female|male>-<n>`; voice.bin = sorted style files F1–F5, M1–M5,
so sid 0–4 female, 5–9 male), Pocket (`pocket-en-<wav name>` from test_wavs/ and voices/), MMS
VITS on characters (`mms-ru`, text lowercased). Kitten/Kokoro/Piper are skipped (they need a
phonemizer: next step, libespeak-ng). 314 voices. `SherpaVoice "<lang>" "<voice>"` per language
(the settings app writes it), voice types MALEn/FEMALEn → n-th voice of that gender of the
first model of the language (Supertonic before MMS and Pocket). Rate → speed 2^(rate/100)
(Supertonic ×1.05, its normal speed). STOP is read in the sherpa progress callback and between
sentences. Ubuntu's onnxruntime + libonnx print hundreds of "Schema error: … already registered"
lines on every model load; stderr goes to /dev/null during the load unless Debug ≥ 2.
Measured (protocol test, busy laptop): "Кафе на углу открылось в девять утра." 2.8 s of speech in
1.3 s, first audio 2.2 s after SPEAK including the model load; "Привет!" 0.9 s.


## Ukrainian, Belarusian and other languages (survey 2026-11-14)
(User: "украинский и белорусский интересны"; light VITS-class models wanted.)

- **Coqui model list** (coqui-ai and the idiap fork): no Russian model except XTTS-v2
  (CPML = non-commercial). Ukrainian: uk/mai/vits (BSD-3), uk/mai/glow-tts (MIT).
  Belarusian: be/common-voice/glow-tts (CC-BY-SA 4.0). Serbian: sr/custom/vits (idiap,
  CC-BY-4.0). VITS (mostly BSD-3, Common Voice data) also for bg, cs, sk, pl, lt, lv, et,
  hr, sl, ro, hu, fi, el, ga, mt.
- **Coqui, the rest of it** (idiap/coqui-ai-TTS, MPL-2.0, maintained: v0.28.0 2026-10,
  pushes daily):
  - **Meta MMS** (fairseq VITS, ~1100 languages incl. ru, uk, be, kk, tt, ba, …; not in
    .models.json, loaded as tts_models/<iso3>/fairseq/vits) — weights CC-BY-NC 4.0, so
    not for the PPA; quality is plain (Bible readings). At most a local/test option.
  - English: en/ljspeech/vits (Apache-2.0) and en/vctk/vits (109 speakers, Apache-2.0;
    VCTK data CC-BY 4.0) — sherpa-onnx ships both (vits-ljs, vits-vctk); Kokoro and Piper
    are better, so nothing to add.
  - Not free: your_tts and en/vctk/fast_pitch (CC BY-NC-ND), XTTS (CPML), jenny (custom).
  - Voice conversion: FreeVC24, kNN-VC, OpenVoice v1/v2 (all MIT) — turn one voice into
    another from a reference clip. Could give a new uk/be voice more timbres (consent
    needed for the reference); not a runtime piece.
  - **Tools for our own voices:** training recipes (VITS, Glow-TTS, …) and ONNX export,
    but the export works only for VITS. alex73's Belarusian model is a Coqui Glow-TTS
    checkpoint, so it loads in this library, but its ONNX export is ours to write
    (or retrain it as VITS on the same data with the Coqui recipe → ONNX → sd_sherpa).
- **sherpa-onnx** already ships 25 of the Coqui VITS models as ONNX (vits-coqui-*-cv /
  css10 / mai, incl. uk-mai, not be) — usable through sd_sherpa as they are.
- **Piper** (182 voices, 58 languages): uk_UA lada x_low, ukrainian_tts medium (3
  speakers), mykyta / oleksa / tetiana high; also kk, ka, hy, sr, pl, cs, sk. No be.
  Piper voices run in sherpa-onnx too (vits-piper-*).
- **Belarusian:**
  - alex73/BelarusianTTSviaCoquiTTS — Glow-TTS + HiFi-GAN checkpoints (.pth), Apache-2.0,
    2026-04; the training data is not stated (ask the author). Needs an ONNX export and a
    small runner: Glow-TTS is not a sherpa family (acoustic model + vocoder, like Matcha).
    The most promising light be voice.
  - Coqui be/common-voice/glow-tts — CC-BY-SA 4.0, Common Voice quality (noisy speakers).
  - archivartaunik/Belarusian_TTS_V2_Final — an XTTS-v2 fine-tune; tagged Apache, but the
    XTTS base is CPML, so effectively non-commercial and heavy. Not for the PPA.
  - YauhenBichel/belarusian-tts (Apache) — plans a Piper be voice; currently uses
    OmniVoice. Watch it.
- **German: Thorsten-Voice** (thorsten-voice.de, added 2026-11-14 by the user) — one
  volunteer (Thorsten Müller) gave his voice since 2019; datasets CC0, free also for
  commercial use, runs offline. Piper: de_DE-thorsten low / medium / high (63–113 MB) and
  thorsten_emotional medium (8 styles, 76 MB). sherpa-onnx ships all four as
  vits-piper-de_DE-thorsten-* (int8 20–35 MB, fp16 35–58 MB) — usable through sd_sherpa
  as they are. Coqui: de/thorsten tacotron2-DDC, tacotron2-DCA, vits (Apache-2.0). The
  obvious first German voice for the PPA.
- **k2-fsa/OmniVoice** — 612M, Qwen3-0.6B base, 600+ languages incl. be, kk, ky, tg, uz,
  tt, ba, hy, ka; zero-shot cloning. About 1 min per 2 s sentence on CPU (per
  belarusian-tts), so not a system voice; at most a source of reference/synthetic data
  for training a light voice.
- **Looked at, not suitable: loudreader/loudr-1** (loudkit; Apache-2.0; a Chatterbox
  derivative; 28 voices in en es de pt fr it pl nl sv da; cloning). No ru/uk/be; 747 MB
  checkpoint, 2.6 GB for ONNX; autoregressive, so slow on CPU. Kokoro and Piper cover the
  same languages much lighter.

Next steps (also in xfce4-speech-settings/PLAN.md): Ukrainian through Piper / sherpa
(no new code); ask alex73 about the data, then export the be model to ONNX.

## ru-speech-bench (meshoklv, read 2026-11-21)
https://github.com/meshoklv/ru-speech-bench (MIT, last commit 9a08cff, 2026-10-22): hands-on cards for 21
Russian speech engines measured in September 2026 for a phone voice agent: licence with source and date,
install, hardware, numbers, pitfalls. Local copy: `_tmp/web/ru-speech-bench-src`. Hardware: Xeon E5-2695 v2
without AVX2 (close to the user's laptop class) and an RTX 3090 Ti. What matters for us:

- **Piper voice licences.** The bench says all four Russian voices were fine-tuned from en_US lessac (Blizzard
  2013 data, "research purposes" only). Checked on rhasspy/piper-voices on 2026-11-21: that held for the
  voices as we package them, but OHF retrained them in November 2026 from new bases: denis (2026-11-09) and
  dmitri (2026-11-12) now come from `rdh` (nl_BE, CC0) and `hifi_tts_92` (Hi-Fi TTS reader 92, CC BY 4.0).
  The new models need Piper >= 1.5 (MRD and SDP, `vowel_clusters` in the config that merge diphthongs into
  single phoneme ids); whether sherpa-onnx's piper front end handles `vowel_clusters` is unchecked.
  Our packaging/piper-voices takes the sherpa-onnx `vits-piper-*` assets, converted from the old models.
  Base models of our packaged voices per current cards: hifi_tts_92 (dmitri, alba, siwis, paola, gosia),
  rdh (denis, thorsten, davefx, darkman), from scratch (ljspeech, issai), Kristin (john, public domain),
  **en_US ryan** (kerstin-low, gilles-low): RyanSpeech is **CC BY-NC-SA 4.0**, so those two are fine-tunes of a
  non-commercial voice under a CC0 label. ruslan is CC BY-NC-SA itself, irina "Unknown" (both already left
  out).
- **Piper's own Russian front end** (espeak-ng, no normalizer): `15 750 рублей` loses "тысяч", `14:00` and
  `25-го` stay unexpanded, `3 сентября` gets a cardinal, homographs have fixed stress. Same cases our
  normalizer must cover (they are in the normbench set).
- **ESpeech-TTS-1 RL-V2** (F5-TTS architecture, Apache-2.0 declared, base checkpoint not stated; original F5
  weights are CC-BY-NC): RTF 0.17-0.36 on a 3090 Ti, 2.8 GB VRAM, 6/6 lines verbatim, stress by RUAccent
  turbo3.1 (`+` before the stressed vowel, also inside numerals). GPU only in practice. The best open Russian
  cloner in the bench.
- **Kokoro on CPU:** float32 with 8 ORT threads RTF 0.77 on that Xeon; **int8 dynamic quantization was ~4x
  slower** than float32 there (no AVX2/VNNI). Concurrent `create()` from threads corrupts output (kokoro-onnx
  #191); espeak-ru ignores the stress mark U+0301. kokoro-ru (zaakirio): OpenRAIL weights, authors report WER
  2.50 % vs 4.38 % for Piper (Whisper round trip, not reproduced).
- **Chatterbox Multilingual:** digits come out as garbage (generation aborts on token repetition), spelled-out
  numbers are fine; its Russian stress needs `russian_text_stresser`, which is not on PyPI; RTF 22-24 on CPU.
- **Qwen3-TTS 1.7B / F5 Russian / XTTS-v2:** reference leakage in cloning (Qwen3 worst), F5 Russian and
  XTTS-v2 non-commercial. CosyVoice 3: Russian unproven. IndexTTS-2: no Russian front end, custom licence.
- **STT** (for the later dictation part): T-one (Apache-2.0, streaming CTC, 300 ms chunks in ~100-120 ms on
  one thread, output alphabet Cyrillic + space only, no digits/Latin; best with 1 ORT thread, the default
  all-core session is slower and 6x more CPU) and GigaAM-v3 through sherpa-onnx (MIT, offline, punctuated
  RNN-T export `csukuangfj/sherpa-onnx-nemo-transducer-punct-giga-am-v3-russian-2025-12-16`). Silero VAD 6.2.1:
  0.5 ms per 32 ms chunk on one thread. Smart Turn v3.2 (BSD-2): weak on their data, defer.
- **Methods worth reusing** (docs/methodology): ASR round-trip with a paired design (same sentence with digits
  and spelled out) to test normalization through a voice; pitch spread in semitones as a "liveliness" proxy;
  utmos rewards cleanliness, not liveliness (Piper irina 3.67 beats ESpeech 3.27).

## To do: wider survey (from the user, 2026-11-12)
- Go through GitHub's "tts" repositories sorted by stars
  (https://github.com/search?q=tts&type=repositories&s=stars&o=desc), page by page:
  for each engine note licence (code and weights), languages (ru/en), CPU speed,
  streaming, last activity, packaging. Add what fits to "Engines by class".
  Part of the big research, together with the Telegram notes above.
