# mkdump.py OUT: reference for the C homograph resolver.  Per sentence: what RUAccent sends to the
# homograph model (texts, hypotheses after _SPACE_BEFORE_PUNCT), the HF token ids of every pair and the
# chosen variant, for tiny2.1 and turbo3.1.  The ё stage is switched off, so the context is the plain text.
# "tok" lines: raw pairs (fuzz line, word) for the tokenizers alone.
import json, sys, random, glob
from ruaccent import RUAccent
from ruaccent.text_preprocessor import TextPreprocessor
from ruaccent.omograph_model import OmographModel, _SPACE_BEFORE_PUNCT
RUAccent._apply_yo = lambda self, sent, words: None
lines = []
for f in ["/w/homo.txt", "/w/hard.txt", "/w/idiot_ch1.txt", "/c/big.txt"]:
    lines += [l.strip() for l in open(f, encoding="utf-8") if l.strip()]
lines += [l.strip() for l in open("/c/core-in.txt", encoding="utf-8")][:1500]
sents = []
for l in lines:
    sents += [s.strip() for s in TextPreprocessor.split_by_sentences(l) if s.strip()]
seen = set(); sents = [s for s in sents if not (s in seen or seen.add(s))]
fuzz = [l.rstrip("\n") for l in open("/c/fuzz.txt", encoding="utf-8")][:400]
out = open(sys.argv[1], "w")
res = {}
for size in ("tiny2.1", "turbo3.1"):
    ra = RUAccent(); ra.load(omograph_model_size=size, use_dictionary=True)
    tok = ra.omograph_model.tokenizer
    cur = []
    orig = OmographModel.classify
    def spy(self, texts, hyps, nums, _o=orig):
        ch = _o(self, texts, hyps, nums)
        t2 = [_SPACE_BEFORE_PUNCT.sub("", t) for t in texts]; k = 0
        for n, c in zip(nums, ch):
            ids = [tok(t2[k + j], hyps[k + j])["input_ids"] for j in range(n)]
            cur.append({"text": t2[k], "hyps": hyps[k:k + n], "ids": ids, "chosen": c}); k += n
        return ch
    OmographModel.classify = spy
    for s in sents:
        cur = []; ra.process_all(s); res.setdefault(s, {})[size] = cur
    OmographModel.classify = orig
    random.seed(1)
    res.setdefault("\0tok", {})[size] = [[a, b, tok(a, b)["input_ids"]] for a in fuzz
                                         for b in [random.choice(["з+амок", "<w>замок</w>", "п+осле", a[:5]])]]
    print(size, "done", flush=True)
for s in sents:
    out.write(json.dumps({"s": s, "tiny": res[s]["tiny2.1"], "turbo": res[s]["turbo3.1"]}, ensure_ascii=False) + "\n")
out.write(json.dumps({"tok": res["\0tok"]}, ensure_ascii=False) + "\n")
print(len(sents), "sentences,", sum(1 for s in sents if res[s]["tiny2.1"]), "with homographs")
