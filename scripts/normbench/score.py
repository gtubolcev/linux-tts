import sys, json, re, collections

def norm(s):
    s = s.lower().replace("ё", "е").replace("-", " ")
    s = re.sub(r"[^\w\s]", " ", s)
    return " ".join(s.split())

def wer(a, b):
    a, b = a.split(), b.split()
    d = list(range(len(b) + 1))
    for i in range(1, len(a) + 1):
        p, d[0] = d[0], i
        for j in range(1, len(b) + 1):
            p, d[j] = d[j], min(d[j] + 1, d[j - 1] + 1, p + (a[i - 1] != b[j - 1]))
    return d[len(b)], max(len(a), 1)

res = {}
for path in sys.argv[1:]:
    d = json.load(open(path, encoding="utf-8"))
    per = collections.defaultdict(lambda: [0, 0])
    errs = words = 0
    bad = []
    oracle = [0, 0]
    for r in d["rows"]:
        if r["exp"].strip() == "?":
            continue
        out = norm(r["out"])
        vars_ = [norm(v) for v in r["exp"].split("|")]
        ok = out in vars_
        per[r["cls"]][0] += ok
        per[r["cls"]][1] += 1
        if r.get("alts"):
            oracle[0] += any(norm(a) in vars_ for a in r["alts"]); oracle[1] += 1
        e, n = min(wer(v, out) for v in vars_)
        errs += e; words += n
        if not ok:
            bad.append((r["cls"], r["src"], r["out"]))
    ms = sorted(r["ms"] for r in d["rows"])
    res[d["which"]] = dict(per=per, wer=errs / max(words, 1), load=d["load_s"], rss=d["rss_mb"],
                           oracle=oracle, med=ms[len(ms) // 2], max=ms[-1], bad=bad)

names = list(res)
classes = sorted({c for v in res.values() for c in v["per"]})
print("| class | " + " | ".join(names) + " |")
print("|---|" + "---|" * len(names))
for c in classes:
    print("| %s | " % c + " | ".join("%d/%d" % tuple(res[n]["per"].get(c, [0, 0])) for n in names) + " |")
tot = lambda n: (sum(v[0] for v in res[n]["per"].values()), sum(v[1] for v in res[n]["per"].values()))
print("| **total exact** | " + " | ".join("**%d/%d**" % tot(n) for n in names) + " |")
print("| best of alternatives | " + " | ".join(("%d/%d" % tuple(res[n]["oracle"])) if res[n]["oracle"][1] else "—" for n in names) + " |")
print("| WER | " + " | ".join("%.1f%%" % (100 * res[n]["wer"]) for n in names) + " |")
print("| ms/phrase median (max) | " + " | ".join("%.1f (%.0f)" % (res[n]["med"], res[n]["max"]) for n in names) + " |")
print("| load s | " + " | ".join("%.1f" % res[n]["load"] for n in names) + " |")
print("| RSS MB | " + " | ".join(str(res[n]["rss"]) for n in names) + " |")
if "-v" in sys.argv[0:1] or True:
    for n in names:
        print("\n## %s misses" % n)
        for c, s, o in res[n]["bad"]:
            print("- [%s] %s → %s" % (c, s, o))
