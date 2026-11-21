#!/usr/bin/python3 -I
"""trace.py RN_SRC IN.jsonl OUT.jsonl: the oracle for the C port of ru_normalizr.

Runs ru_normalizr (our fork, Python sources in RN_SRC) with NormalizeOptions.tts(), as
tts-normalize does, on each {"id", "text"} record of IN.jsonl and writes
{"id", "in", "out", "steps"} records. steps lists every pipeline method call in order as
[depth, name, in, out]. "in" is null when it equals the previous call's out, and "out" is
null when the call changed nothing. The C port is checked against the outputs and also
stage by stage.
"""
import functools
import json
import sys
import time

# The PPA ships pure-Python dawg2-python, not the DAWG2 C extension, and the prefix
# matchers of the two order their results differently, which can change parse order.
sys.modules["dawg"] = None
sys.path.insert(0, sys.argv[1])
from ru_normalizr import NormalizeOptions, Normalizer  # noqa: E402
from ru_normalizr.normalizer import PipelineNormalizer  # noqa: E402

STEPS = []
DEPTH = [0]


def wrap(name, fn):
    @functools.wraps(fn)
    def w(self, text, *a, **kw):
        rec = [DEPTH[0], name, text, None]
        STEPS.append(rec)
        DEPTH[0] += 1
        try:
            out = fn(self, text, *a, **kw)
        finally:
            DEPTH[0] -= 1
        rec[3] = out
        return out
    return w


for n in list(vars(PipelineNormalizer)):
    if (n.startswith("run_") or n.startswith("_run_")) and n != "run_stage":
        setattr(PipelineNormalizer, n, wrap(n, getattr(PipelineNormalizer, n)))

norm = Normalizer(NormalizeOptions.tts())
t0 = time.monotonic()
n = 0
with open(sys.argv[2], encoding="utf-8") as fi, open(sys.argv[3], "w", encoding="utf-8") as fo:
    for line in fi:
        r = json.loads(line)
        STEPS.clear()
        try:
            out = norm.normalize(r["text"])
        except Exception as e:  # the C port must not crash where Python raised
            out = None
            r["error"] = "%s: %s" % (type(e).__name__, e)
        steps, prev = [], None
        for d, name, i, o in STEPS:
            steps.append([d, name, None if i == prev else i, None if o == i else o])
            prev = o
        rec = {"id": r["id"], "in": r["text"], "out": out, "steps": steps}
        if "error" in r:
            rec["error"] = r["error"]
        fo.write(json.dumps(rec, ensure_ascii=False) + "\n")
        n += 1
print("%d records, %.1f s" % (n, time.monotonic() - t0), file=sys.stderr)
