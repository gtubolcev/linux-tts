#!/usr/bin/python3 -I
"""mk-parity-texts.py GOLDEN.jsonl.zst OUT.bin: texts for re-parity: every distinct string
in the goldens (inputs, outputs, every pipeline step), plus all code points in pieces of 2000."""
import json
import subprocess
import sys

seen = {}
raw = subprocess.run(["zstdcat", sys.argv[1]], capture_output=True, check=True).stdout
for line in raw.decode("utf-8").splitlines():
    r = json.loads(line)
    for s in [r["in"], r["out"]] + [x for st in r["steps"] for x in st[2:4]]:
        if isinstance(s, str):
            seen.setdefault(s, None)
# all code points, in pieces: one 1.1M-character text makes some patterns quadratic in Python
allcp = "".join(chr(c) for c in range(0x110000) if not 0xD800 <= c <= 0xDFFF)
for i in range(0, len(allcp), 2000):
    seen[allcp[i:i + 2000]] = None
with open(sys.argv[2], "wb") as f:
    for s in seen:
        b = s.encode("utf-8")
        f.write(b"%d\n" % len(b) + b)
print(len(seen), "texts", file=sys.stderr)
