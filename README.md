# linux-tts

Native [speech-dispatcher](https://github.com/brailcom/speechd) output modules for
modern neural text-to-speech engines, running locally on the CPU. Each module keeps its
model loaded, instead of starting a process per sentence like a generic (shell
pipeline) module.

| Module | Engine | Languages |
|---|---|---|
| `sherpa/` — `sd_sherpa-onnx` | [sherpa-onnx](https://github.com/k2-fsa/sherpa-onnx): Supertonic 3, [Piper](https://github.com/OHF-Voice/piper1-gpl) voices (packages in `packaging/piper-voices/`, the others through the downloader of xfce4-speech-settings), Pocket TTS, MMS | many (Supertonic), 40+ (Piper), en (Pocket), ru (MMS) |
| `vosk/` — `sd_vosk` | [Vosk TTS](https://github.com/alphacep/vosk-tts) 0.9-multi, a C port of `vosk_tts` | ru |
| `kokoro/` — `sd_kokoro` | Russian Kokoro ([zaakirio/kokoro-ru](https://huggingface.co/zaakirio/kokoro-ru)) with a C port of its G2P | ru |
| `teratts/` — `sd_teratts` | [TeraTTS](https://huggingface.co/TeraTTS) (OpenRAIL-M) with a stress dictionary | ru, en |
| `silero/` — `sd_silero` | [Silero](https://github.com/snakers4/silero-models) v5_6_ru through `silero-server` on the ONNX port [silero-tts-onnx](https://github.com/some1-in-here/silero-tts-onnx); the model (CC BY-NC-SA, non-commercial) is installed separately into `~/.local/share/silero-tts/` | ru |

`sd_sherpa-onnx-vulkan-rs` is the same module on the graphics
card through Vulkan ([onnx-vulkan-rs](https://github.com/0xshawn/onnx-vulkan-rs)), with
only the models it can run (Piper, MMS).

`extra/` holds modules that are not packaged; they can be built by hand. `extra/zonos3/`
(`sd_zonos3`, [ZONOS3-Mini](https://github.com/Zyphra/ZONOS3-Mini) through its server) is
there until its model stops inventing words in about a quarter of the phrases.

Every directory has `build.sh` (in docker) and `build-deb.sh`. The models are packaged
separately; see each `build-deb.sh`. Measurements and notes on the engines:
[RESEARCH-tts-engines.md](RESEARCH-tts-engines.md). How the modules, sherpa-onnx, ONNX Runtime,
ggml and the graphics card fit together: [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md).

A settings dialog for Xfce: [xfce4-speech-settings](https://github.com/gtubolcev/xfce4-speech-settings).

Status: early. Packages for Ubuntu 26.04: `sudo add-apt-repository ppa:gtubolcev/linux-tts`
(the engines, the modules, speech-dispatcher and RHVoice with fixes; see
[packaging/README.md](packaging/README.md)).

## License

MIT, see [LICENSE](LICENSE). `vosk/vosk.c` ports the Python `vosk_tts` package and
`kokoro/ru-g2p.c` ports `ru_g2p.py` of kokoro-ru, both Apache-2.0. The models have
licenses of their own (OpenRAIL-M, Apache-2.0, CC-BY-4.0, CC-BY-NC-4.0 …), see the
model packages. The Silero models and silero-tts-onnx are CC BY-NC-SA 4.0 and are
not packaged here.
