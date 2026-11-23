#!/usr/bin/env python3
"""model_spec for a TeraTTS model dir (family supertonic, Tera voices).
usage: tera-spec.py SUPERTONIC_SPEC MODEL_DIR"""
import json, sys
from pathlib import Path

spec = json.load(open(sys.argv[1]))
model = Path(sys.argv[2])
voices = sorted(p.stem for p in (model / "voice_styles").glob("*.json"))

spec["display_name"] = "TeraTTS v2"
spec["description"] = ("TeraTTS v2 (Supertonic architecture, distilled sampler), "
                       "Russian and English, Open RAIL-M.")
spec["languages"] = ["ru", "en"]
spec["capabilities"] = {"tts": ["built_in_voices"]}
req = spec["options"]["request"]
for o in req:
    if o["name"] == "voice_id":
        o["values"], o["default"] = voices, "ru_f1" if "ru_f1" in voices else voices[0]
req.append({"name": "raw_text", "type": "bool", "default": False, "required": False,
            "description": "The text is prepared: language tags, stress marks; only NFKD."})
spec["ui"]["recommended_package"] = "teratts_v2_safetensors"
spec["packages"] = [{
    "id": "teratts_v2_safetensors", "display_name": "TeraTTS v2 Safetensors", "default": True,
    "format": "safetensors", "precision": "native", "target_directory": "teratts-v2",
    "files": ["config/tts.json", "config/unicode_indexer.json", "ggml/supertonic.safetensors"]
             + [f"voice_styles/{v}.json" for v in voices],
}]
spec.pop("package_defaults", None)
files = {"tts_config": "model:config/tts.json", "unicode_indexer": "model:config/unicode_indexer.json"}
files.update({f"voice_style_{v}": f"model:voice_styles/{v}.json" for v in voices})
spec["sources"] = [{"format": "safetensors", "roots": {"model": "."}, "files": files,
                    "tensors": {"weights": "model:ggml/supertonic.safetensors"}}]
(model / "model_spec.json").write_text(json.dumps(spec, indent=2, ensure_ascii=False) + "\n")
print("voices:", voices)

# sd_audiocpp's manifest
ini = ["[model]", "family=supertonic", "file=.", "name=teratts", "language=ru",
       "model-spec=model_spec.json", "phonemizer=teratts", "speed-base=1.05", ""]
for v in voices:
    f = v.split("_")
    lang = "en" if f[0] == "eng" else f[0]
    ini += [f"[voice {'-'.join(f[1:])}]", f"voice-id={v}", f"language={lang}",
            "gender=" + ("female" if f[1].startswith("f") else "male"), ""]
(model / "audiocpp.ini").write_text("\n".join(ini))
