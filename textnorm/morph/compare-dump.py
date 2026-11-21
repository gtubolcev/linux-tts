#!/usr/bin/python3 -I
"""compare-dump.py REF.jsonl C.jsonl [N]: differences between morph-dump.py and morph-dump."""
import collections
import json
import sys

ref, got = open(sys.argv[1], encoding="utf-8"), open(sys.argv[2], encoding="utf-8")
show = int(sys.argv[3]) if len(sys.argv) > 3 else 5
bad = collections.Counter()
ex = collections.defaultdict(list)
total = 0
for lr, lg in zip(ref, got):
    r, g = json.loads(lr), json.loads(lg)
    total += 1
    if r["w"] != g["w"]:
        sys.exit("word lists differ at line %d" % total)
    for f in ("k", "p", "l", "i"):
        if r[f] != g[f]:
            bad[f] += 1
            if len(ex[f]) < show:
                ex[f].append((r["w"], r[f], g[f]))
print("%d words; mismatches: %s" % (total, dict(bad) or "none"))
for f, xs in ex.items():
    for w, a, b in xs:
        if f in ("l", "i"):
            # first differing element only
            for k, (x, y) in enumerate(zip(a, b)):
                if x != y:
                    if isinstance(x, list) and isinstance(y, list) and len(x) == len(y):
                        for j, (u, v) in enumerate(zip(x, y)):
                            if u != v:
                                x, y = (j, u), (j, v)
                                break
                    a, b = (k, x), (k, y)
                    break
            else:
                a, b = ("len", len(a)), ("len", len(b))
        print("[%s] %r\n  ref %s\n  c   %s" % (f, w, json.dumps(a, ensure_ascii=False)[:600],
                                           json.dumps(b, ensure_ascii=False)[:600]))
