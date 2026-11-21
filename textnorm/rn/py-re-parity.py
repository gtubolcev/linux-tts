#!/usr/bin/python3 -I
"""py-re-parity.py PATTERNS.jsonl TEXTS.bin [PATTERN_NO]: Python's side of re-parity.c."""
import json
import re
import sys

recs = [json.loads(line) for line in open(sys.argv[1], encoding="utf-8")]
data = open(sys.argv[2], "rb").read()
texts, i = [], 0
while i < len(data):
    j = data.index(b"\n", i)
    n = int(data[i:j])
    texts.append(data[j + 1:j + 1 + n].decode("utf-8"))
    i = j + 1 + n
only = int(sys.argv[3]) if len(sys.argv) > 3 else -1
for pno, rec in enumerate(recs):
    if only >= 0 and pno != only:
        continue
    rx = re.compile(rec["py"], rec["flags"])
    h = 0xcbf29ce484222325
    count = 0
    for t, s in enumerate(texts):
        for m in rx.finditer(s):
            count += 1
            parts = ["%d:" % t] + [" %d,%d" % m.span(g) for g in range(rx.groups + 1)] + [";"]
            if only >= 0:
                print("".join(parts[:-1]))
            for ch in "".join(parts).encode():
                h = ((h ^ ch) * 0x100000001b3) & 0xFFFFFFFFFFFFFFFF
    if only < 0:
        print("%d %d %016x" % (pno, count, h), flush=True)
