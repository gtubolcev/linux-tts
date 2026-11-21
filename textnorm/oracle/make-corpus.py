#!/usr/bin/python3 -I
"""make-corpus.py RN_REPO LINUX_TTS OUT_DIR [EXTRA.txt…]: input corpus for the oracle.

Writes OUT_DIR/core.jsonl, which is committed (MIT/PD/own sources only):
- every Russian string literal of ru_normalizr's tests (MIT) and tests/data/*.json;
- our normbench and norm-test texts;
- a fixed sample of the public-domain Wikisource books in RN_REPO/benchmarks/corpus, cut into
  pieces of at most 300 characters, mostly with digits, Latin letters, abbreviations or capitals.
It also writes OUT_DIR/big.jsonl, which is not committed: all book lines plus the EXTRA
files (e.g. the 4pda topic), cut into chunks of at most 1000 characters.
"""
import ast
import json
import os
import random
import re
import sys

RN, LT, OUT = sys.argv[1], sys.argv[2], sys.argv[3]
EXTRA = sys.argv[4:]
CYR = re.compile(r"[А-Яа-яЁё]")
HOT = re.compile(r"[0-9A-Za-z№%$€§]|[А-ЯЁ]{2,}|\b[а-яё]{1,4}\.(?=\s*[а-яё0-9])|[IVXLC]{2,}")


def chunks(text, limit=1000):
    text = text.strip()
    while len(text) > limit:
        cut = max(text.rfind(". ", 0, limit), text.rfind("\n", 0, limit))
        cut = cut + 1 if cut > limit // 3 else limit
        yield text[:cut].strip()
        text = text[cut:].strip()
    if text:
        yield text


def test_strings():
    seen = []
    tdir = os.path.join(RN, "tests")
    for name in sorted(os.listdir(tdir)):
        p = os.path.join(tdir, name)
        if name.endswith(".py"):
            for node in ast.walk(ast.parse(open(p, encoding="utf-8").read())):
                if isinstance(node, ast.Constant) and isinstance(node.value, str):
                    s = node.value
                    if CYR.search(s) and 2 < len(s) < 2000 and "\x00" not in s:
                        seen.append(("rn-test:" + name, s))
    ddir = os.path.join(tdir, "data")
    for name in sorted(os.listdir(ddir)):
        def walk(x):
            if isinstance(x, str):
                if CYR.search(x) and 2 < len(x) < 2000:
                    seen.append(("rn-data:" + name, x))
            elif isinstance(x, dict):
                for v in x.values():
                    walk(v)
            elif isinstance(x, list):
                for v in x:
                    walk(v)
        if name.endswith(".json"):
            walk(json.load(open(os.path.join(ddir, name), encoding="utf-8")))
    return seen


def ours():
    out = []
    for line in open(os.path.join(LT, "scripts/normbench/ref-ru.tsv"), encoding="utf-8"):
        if line.strip() and not line.startswith("#"):
            out.append(("normbench", line.split("\t")[1]))
    for line in open(os.path.join(LT, "scripts/norm-test/texts-ru.txt"), encoding="utf-8"):
        if line.strip() and not line.startswith("#"):
            out.append(("norm-test", line.split("\t", 1)[-1].strip()))
    return out


def books(limit=1000):
    d = os.path.join(RN, "benchmarks/corpus")
    for name in sorted(os.listdir(d)):
        if name.endswith(".txt"):
            with open(os.path.join(d, name), encoding="utf-8") as f:
                for line in f:
                    for c in chunks(line, limit):
                        yield "book:" + name[:-4], c


def write(path, items):
    seen = set()
    n = 0
    with open(path, "w", encoding="utf-8") as f:
        for src, text in items:
            if text in seen:
                continue
            seen.add(text)
            f.write(json.dumps({"id": "%s:%d" % (src, n), "text": text}, ensure_ascii=False) + "\n")
            n += 1
    print(path, n, file=sys.stderr)


os.makedirs(OUT, exist_ok=True)
rng = random.Random(20261121)
hot, cold = [], []
for src, t in books(300):
    (hot if HOT.search(t) else cold).append((src, t))
core_books = rng.sample(hot, min(8000, len(hot))) + rng.sample(cold, min(500, len(cold)))
write(os.path.join(OUT, "core.jsonl"), ours() + test_strings() + core_books)


def big():
    yield from books()
    for p in EXTRA:
        with open(p, encoding="utf-8") as f:
            for para in re.split(r"\n\s*\n", f.read()):
                if CYR.search(para):
                    for c in chunks(para):
                        yield "extra:" + os.path.basename(p), c


write(os.path.join(OUT, "big.jsonl"), big())
