# ruaccent-yo-dump.py OUT: reference for the ё stage of the C homograph resolver
# (ru-homograph-test --yo).  Per sentence: the text the ё model gets (model_text.lower()), its token ids
# and offsets, and for every word of yo_homographs.json.gz its start and RUAccent's label (YO / NO_YO).
# "tok" lines: raw fuzz lines with ids and offsets, for the tokenizer alone.
# Run in the claude-stressvote image like mkdump.py (same /w and /c files).
import json, sys
from ruaccent import RUAccent
from ruaccent.ruaccent import _SpanLookup
from ruaccent.text_preprocessor import TextPreprocessor

lines = []
for f in ["/w/yo_in.txt", "/w/homo.txt", "/w/hard.txt", "/w/idiot_ch1.txt", "/c/big.txt"]:
    lines += [l.strip() for l in open(f, encoding="utf-8") if l.strip()]
lines += [l.strip() for l in open("/c/core-in.txt", encoding="utf-8")][:1500]
sents = []
for l in lines:
    sents += [s.strip() for s in TextPreprocessor.split_by_sentences(l) if s.strip()]
seen = set(); sents = [s for s in sents if not (s in seen or seen.add(s))]
fuzz = [l.rstrip("\n") for l in open("/c/fuzz.txt", encoding="utf-8")][:400]

ra = RUAccent(); ra.load(omograph_model_size="tiny2.1", use_dictionary=True)
m = ra.yo_homograph_model
tok = m.tokenizer
out = open(sys.argv[1], "w")
n_words = n_yo = 0
for s in sents:
    sent = TextPreprocessor.analyze(s)
    lower = sent.model_text.lower()
    enc = tok(lower, return_offsets_mapping=True, truncation=True, max_length=512)
    words = []
    if "е" in lower:
        yo = _SpanLookup(m.predict_yo_homographs(lower))
        for t in sent.tokens:
            w = t.text.lower()
            if w in ra.yo_homographs:
                words.append([t.start, w, yo.label(t.start, "NO_YO")])
    n_words += len(words); n_yo += sum(1 for w in words if w[2] == "YO")
    out.write(json.dumps({"yo": s, "text": lower, "ids": enc["input_ids"],
                          "offsets": enc["offset_mapping"], "words": words}, ensure_ascii=False) + "\n")
for a in fuzz:
    enc = tok(a, return_offsets_mapping=True, truncation=True, max_length=512)
    out.write(json.dumps({"tok": a, "ids": enc["input_ids"], "offsets": enc["offset_mapping"]},
                         ensure_ascii=False) + "\n")
print(len(sents), "sentences,", n_words, "ё-homographs,", n_yo, "YO")
