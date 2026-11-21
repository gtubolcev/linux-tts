#!/usr/bin/env python3
# en-tagger-export - writes the data of en-tagger.c from spaCy's
# en_core_web_sm 3.8 (MIT, Explosion): the tok2vec and tagger weights as
# en-tagger.bin ("ENTAGv1\0", then little-endian float32: the six hash
# embeddings, the embedding maxout and its layer norm, four maxout+norm
# layers, the softmax), spaCy's norms (BASE_NORMS over the lexeme_norm table
# of spacy-lookups-data), the tokenizer's special cases, its regexes and
# spaCy's symbol ids.
#
#   python3 -I en-tagger-export.py <out dir> <spacy_lookups_data-1.0.5 .whl>
#
# with spacy 3.8 and en_core_web_sm 3.8.0 installed.  The labels
# (en-tagger-labels.txt) must equal LABELS in en-tagger.c.
#
# Copyright (C) 2026 Grigory Tuboltsev
# SPDX-License-Identifier: GPL-3.0-or-later
import spacy, sys, re, numpy as np, json, gzip, zipfile
from spacy.attrs import ORTH, NORM
from spacy.lang.norm_exceptions import BASE_NORMS
out = sys.argv[1]
nlp = spacy.load("en_core_web_sm")
t2v = nlp.get_pipe("tok2vec").model
tg = nlp.get_pipe("tagger").model
P = {(n.id, p): n.get_param(p) for n in t2v.walk() for p in n.param_names if n.has_param(p)}
sm = [n for n in tg.walk() if n.name == "softmax"][0]
order = [(1, "E"), (4, "E"), (7, "E"), (10, "E"), (13, "E"), (16, "E"), (19, "W"), (19, "b"), (20, "G"), (20, "b")]
for w, l in [(32, 33), (40, 41), (48, 49), (56, 57)]:
    order += [(w, "W"), (w, "b"), (l, "G"), (l, "b")]
seeds = [n.attrs["seed"] for n in t2v.walk() if n.name == "hashembed"]
assert seeds == [8, 9, 10, 11, 12, 13], seeds
with open(out + "/en-tagger.bin", "wb") as f:
    f.write(b"ENTAGv1\0")
    for k in order:
        f.write(np.ascontiguousarray(P[k], dtype="<f4").tobytes())
    f.write(np.ascontiguousarray(sm.get_param("W"), dtype="<f4").tobytes())
    f.write(np.ascontiguousarray(sm.get_param("b"), dtype="<f4").tobytes())
with open(out + "/en-tagger-labels.txt", "w") as f:
    f.write("\n".join(nlp.get_pipe("tagger").labels) + "\n")
t = nlp.vocab.lookups.get_table("lexeme_norm")
z = zipfile.ZipFile(sys.argv[2])
d = json.loads(gzip.decompress(z.read("spacy_lookups_data/data/en_lexeme_norm.json.gz")))
assert len(d) == len(t) and all(k in t and t[k] == v for k, v in d.items())
norms = dict(BASE_NORMS)
norms.update(d)  # add_lookups checks BASE_NORMS first, then the table
for k, v in BASE_NORMS.items(): norms[k] = v
with open(out + "/en-tagger-norm.tsv", "w") as f:
    for k in sorted(norms):
        assert "\t" not in k + norms[k] and "\n" not in k + norms[k]
        f.write(k + "\t" + norms[k] + "\n")
with open(out + "/en-tagger-rules.tsv", "w") as f:
    for k, v in sorted(nlp.tokenizer.rules.items()):
        if any(c in k for c in "\t\n") : continue
        f.write(k + "\t" + "\t".join(x[ORTH] + "\x1f" + x.get(NORM, "") for x in v) + "\n")
# spaCy's symbols: their strings map to these ids, not to hashes
from spacy.symbols import IDS
with open(out + "/en-tagger-symbols.tsv", "w") as f:
    for k, v in sorted(IDS.items(), key=lambda kv: kv[1]):
        if k and "\t" not in k and "\n" not in k:
            assert nlp.vocab.strings[k] == v, k
            f.write(k + "\t" + str(v) + "\n")
# the tokenizer's regexes
assert nlp.tokenizer.token_match is None
with open(out + "/en-tagger-tokenizer.txt", "w") as f:
    for name in ("prefix_search", "suffix_search", "infix_finditer", "url_match"):
        p = getattr(nlp.tokenizer, name).__self__.pattern
        if p.startswith("(?u)"): p = p[4:]
        # PCRE spells \uXXXX and \UXXXXXXXX as \x{...}
        p = re.sub(r"(?<!\\)\\u([0-9A-Fa-f]{4})", r"\\x{\1}", p)
        p = re.sub(r"(?<!\\)\\U([0-9A-Fa-f]{8})", r"\\x{\1}", p)
        assert "\n" not in p and "\t" not in p
        f.write(name.split("_")[0] + "\t" + p + "\n")
print("ok", sum(P[k].size for k in order) + sm.get_param("W").size + 50, file=sys.stderr)
