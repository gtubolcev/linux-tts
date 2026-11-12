# linux-tts

Native [speech-dispatcher](https://github.com/brailcom/speechd) output modules for
modern neural text-to-speech engines, running locally on the CPU. Each module keeps its
model loaded, instead of starting a process per sentence like a generic (shell
pipeline) module.

| Module | Engine | Languages |
|---|---|---|
| `sherpa/` — `sd_sherpa` | [sherpa-onnx](https://github.com/k2-fsa/sherpa-onnx): Supertonic 3, Pocket TTS, MMS | many (Supertonic), en (Pocket), ru (MMS) |
| `vosk/` — `sd_vosk` | [Vosk TTS](https://github.com/alphacep/vosk-tts) 0.9-multi, a C port of `vosk_tts` | ru |
| `kokoro/` — `sd_kokoro` | Russian Kokoro ([zaakirio/kokoro-ru](https://huggingface.co/zaakirio/kokoro-ru)) with a C port of its G2P | ru |
| `zonos3/` — `sd_zonos3` | [ZONOS3-Mini](https://github.com/Zyphra/ZONOS3-Mini) through its server, Vulkan GPU with CPU fallback | en, ru, … |

Every directory has `build.sh` (in docker) and `build-deb.sh`. The models are packaged
separately; see each `build-deb.sh`. Measurements and notes on the engines:
[RESEARCH-tts-engines.md](RESEARCH-tts-engines.md).

A settings dialog for Xfce: [xfce4-speech-settings](https://github.com/gtubolcev/xfce4-speech-settings).

Status: early, local packages only; a PPA is planned.

## License

MIT, see [LICENSE](LICENSE). `vosk/vosk.c` ports the Python `vosk_tts` package and
`kokoro/ru-g2p.c` ports `ru_g2p.py` of kokoro-ru, both Apache-2.0. The models have
licenses of their own (OpenRAIL-M, Apache-2.0, CC-BY-4.0, CC-BY-NC-4.0 …), see the
model packages.
