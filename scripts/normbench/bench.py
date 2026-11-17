import sys, time, re, resource, json
which, ref = sys.argv[1], sys.argv[2]
rows = [l.rstrip("\n").split("\t") for l in open(ref, encoding="utf-8") if l.strip() and not l.startswith("#")]
t0 = time.perf_counter()
if which == "rn":
    from ru_normalizr import Normalizer, NormalizeOptions
    n = Normalizer(NormalizeOptions.tts())
    f = n.normalize
elif which == "nemo":
    from nemo_text_processing.text_normalization.normalize_with_audio import NormalizerWithAudio
    n = NormalizerWithAudio(input_case="cased", lang="ru", cache_dir="/work/nemo-cache")
    ALTS = {}
    def f(t):
        o = sorted(n.normalize(t, n_tagged=50, punct_post_process=True))
        ALTS[t] = o
        return o[0] if o else ""
elif which == "runorm":
    from runorm import RUNorm
    n = RUNorm(); n.load(model_size="small", device="cpu")
    f = n.norm
elif which == "byt5":
    from transformers import AutoTokenizer, AutoModelForSeq2SeqLM
    tok = AutoTokenizer.from_pretrained("intx82/byt5-textnorm-ru", use_fast=False)
    mdl = AutoModelForSeq2SeqLM.from_pretrained("intx82/byt5-textnorm-ru")
    def f(t):
        i = tok(t, return_tensors="pt", truncation=True, max_length=200)
        o = mdl.generate(**i, max_length=384, num_beams=1, do_sample=False)
        return tok.decode(o[0], skip_special_tokens=True)
elif which == "saarus":
    from transformers import GPT2Tokenizer, T5ForConditionalGeneration
    tok = GPT2Tokenizer.from_pretrained("saarus72/russian_text_normalizer", eos_token="</s>")
    mdl = T5ForConditionalGeneration.from_pretrained("saarus72/russian_text_normalizer")
    SPAN = re.compile(r"\d+(?:[.,:]\d+)*|[A-Za-z][A-Za-z0-9'&.-]*")
    def group(m):
        x = m.group(0)
        if x.isdigit() and len(x) > 3:
            x = " ".join(x[max(0, k - 3):k] for k in range(len(x), 0, -3)[::-1])
        return x
    def f(t):
        spans = list(SPAN.finditer(t))
        if not spans:
            return t
        parts, last = [], 0
        for k, m in enumerate(spans):
            parts.append(t[last:m.start()] + "[" + group(m) + "]<extra_id_%d>" % k)
            last = m.end()
        parts.append(t[last:])
        i = tok("<SC1>" + "".join(parts), return_tensors="pt")
        o = mdl.generate(**i, max_new_tokens=256, eos_token_id=tok.eos_token_id, early_stopping=True)
        dec = tok.decode(o[0])
        got = {}
        for k in range(len(spans)):
            mm = re.search(r"<extra_id_%d>(.*?)(?=<extra_id_|</s>|$)" % k, dec, re.S)
            got[k] = mm.group(1).strip() if mm else spans[k].group(0)
        res, last = [], 0
        for k, m in enumerate(spans):
            res.append(t[last:m.start()] + got[k]); last = m.end()
        res.append(t[last:])
        return "".join(res)
load = time.perf_counter() - t0
f("Прогрев 1 2 3.")
out = []
total = 0.0
for cls, src, exp in rows:
    t = time.perf_counter()
    try:
        r = f(src)
    except Exception as e:
        r = "ERROR: %s" % e
    dt = time.perf_counter() - t
    total += dt
    out.append({"cls": cls, "src": src, "exp": exp, "out": r, "ms": round(dt * 1000, 2), "alts": globals().get("ALTS", {}).get(src, [])})
json.dump({"which": which, "load_s": round(load, 2), "total_ms": round(total * 1000, 1),
           "rss_mb": resource.getrusage(resource.RUSAGE_SELF).ru_maxrss // 1024, "rows": out},
          open("/work/out-%s.json" % which, "w"), ensure_ascii=False, indent=1)
print(which, "load", round(load, 2), "s; total", round(total * 1000, 1), "ms; rss",
      resource.getrusage(resource.RUSAGE_SELF).ru_maxrss // 1024, "MB")
