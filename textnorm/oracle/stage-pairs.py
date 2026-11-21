#!/usr/bin/python3 -I
"""stage-pairs.py GOLDEN.jsonl[.zst] STAGE OUT.bin: the distinct (input, output) pairs of one
pipeline stage (e.g. run_urls) from the goldens, for checking a C stage alone. STAGE "A>B"
gives the gap between two stages instead: A's output and B's input (e.g. run_urls>run_math
for the path and overlong-number protection that runs unrecorded in between).

OUT.bin holds records "<bytes>\\n<utf-8 input>" followed by "<bytes>\\n<utf-8 output>",
in first-seen order. Steps store "in" as null when it equals the previous call's output and
"out" as null when unchanged (trace.py)."""
import json
import subprocess
import sys

if sys.argv[1].endswith(".zst"):
    raw = subprocess.run(["zstdcat", sys.argv[1]], capture_output=True, check=True).stdout
else:
    raw = open(sys.argv[1], "rb").read()
first, _, second = sys.argv[2].partition(">")
pairs = {}
for line in raw.decode("utf-8").splitlines():
    r = json.loads(line)
    prev = gap = None
    for _d, name, i, o in r["steps"]:
        i = prev if i is None else i
        o = i if o is None else o
        prev = o
        if second and gap is not None and name == second:
            pairs.setdefault(gap, i)
            gap = None
        if name == first:
            if second:
                gap = o
            else:
                pairs.setdefault(i, o)
with open(sys.argv[3], "wb") as f:
    for i, o in pairs.items():
        for s in (i, o):
            b = s.encode("utf-8")
            f.write(b"%d\n" % len(b) + b)
print("%d pairs, %d changed" % (len(pairs), sum(i != o for i, o in pairs.items())),
      file=sys.stderr)
