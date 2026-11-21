#!/usr/bin/python3 -I
"""e2e-pairs.py GOLDEN.jsonl[.zst] OUT.bin: the distinct (in, out) pairs of the whole
pipeline from the goldens, in the stage-pairs.py format, for rn-stage-test normalize."""
import json
import subprocess
import sys

if sys.argv[1].endswith(".zst"):
    raw = subprocess.run(["zstdcat", sys.argv[1]], capture_output=True, check=True).stdout
else:
    raw = open(sys.argv[1], "rb").read()
pairs = {}
for line in raw.decode("utf-8").splitlines():
    r = json.loads(line)
    pairs.setdefault(r["in"], r["out"])
with open(sys.argv[2], "wb") as f:
    for i, o in pairs.items():
        for s in (i, o):
            b = s.encode("utf-8")
            f.write(b"%d\n" % len(b) + b)
print(len(pairs), "pairs", sum(i != o for i, o in pairs.items()), "changed", file=sys.stderr)
