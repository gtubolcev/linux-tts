# runtime stress dictionaries for common/ru-stress.c
#   ru-stress.tsv    user verdicts + Wiktionary (CC-BY-SA-4.0): shippable
#   ru-stress-jn.tsv Jugger-Naut words missing above (no licence: local only)
# line: key<TAB>variant[,variant...]; key: lower case, ё written е; sorted by bytes
# variant: lower-case form with U+0301 after the stressed vowel (ё alone marks
# itself), then "/" and flags if any: n proper name, r rare, d default
import sys, json, collections
WIKT, JN, VOSK, VERD, DIS, OUT, OUTJ, FREQ = sys.argv[1:9]
AC = "́"; VOW = "аеёиоуыэюя"
def bare(s): return s.replace(AC, "")
def key(s): return bare(s.lower()).replace("ё", "е")
def canon(st):
    st = st.lower().replace("ё" + AC, "ё")
    if st.count(AC) > 1: return None
    for i, c in enumerate(st):
        if c == AC and (i == 0 or st[i-1] not in VOW): return None
    b = bare(st)
    if AC not in st and "ё" not in b: return None
    if sum(c in VOW for c in b) < 2: return None
    return st
def spos(st):  # index of the stressed vowel among the vowels
    n = -1; last = None
    for c in st:
        if c in VOW: n += 1; last = c
        if c == AC: return n
    b = bare(st); vs = [c for c in b if c in VOW]
    return vs.index("ё") if "ё" in vs else -1
# key -> {(form, proper): rare_all}
W = collections.defaultdict(dict)
rows = []; PL = set()
for l in open(WIKT, encoding="utf-8"):
    f = l.rstrip("\n").split("\t")
    st = canon(f[1])
    if st is None: continue
    fl = f[6] if len(f) > 6 else ""
    if "n" in fl: PL.add(f[2])
    rows.append((st, f[2], f[5], fl, f[3], f[4]))
# lemma frequencies of the Russian National Corpus (Lyashevskaya, Sharov
# 2009), only to order the readings: the more frequent lemma first
POS = {"noun": ("s",), "name": ("s",), "verb": ("v",), "adj": ("a", "apro", "anum"),
       "adv": ("adv", "advpro"), "particle": ("part",), "pron": ("spro", "apro", "advpro"),
       "num": ("num", "anum"), "conj": ("conj",), "prep": ("pr",), "intj": ("intj",),
       "participle": ("v",), "det": ("apro",)}
F = {}
for l in open(FREQ, encoding="utf-8"):
    f = l.rstrip("\n").split("\t")
    try: F[(f[0].lower().replace("ё", "е"), f[1])] = float(f[2])
    except (IndexError, ValueError): pass
SC = collections.defaultdict(float)  # (key, form) -> frequency of its most frequent lemma
RR = collections.defaultdict(lambda: [0, 0, 0, 0, 0])  # (key, form, proper) -> rows, rare rows, non-canonical rows, rare ones
CMP = set()  # comparatives: у́же is узкий, not the headword уже
for st, lem, src, fl, pos, tags in rows:
    if "comparative" in tags or "superlative" in tags: CMP.add((key(st), st))
for st, lem, src, fl, pos, tags in rows:
    p = "n" in fl or (src == "ru" and lem in PL)
    r = RR[(key(st), st, p)]; nc = tags != "canonical" and "canonical," not in tags
    r[0] += 1; r[1] += "r" in fl; r[2] += nc; r[3] += nc and "r" in fl
    r[4] += not nc and lem == bare(st) and "r" not in fl  # the headword itself
    if not ((key(st), st) in CMP and lem == bare(st)):
        sc = max((F.get((lem.replace("ё", "е"), x), 0.0) for x in (("s.PROP",) if p else POS.get(pos, ()))), default=0.0)
        SC[(key(st), st, p)] = max(SC[(key(st), st, p)], sc)
for (k, st, p), (n, nr, nc, ncr, head) in RR.items():
    # rare when every row is, or every row of an inflection (the headword
    # rows of form pages say nothing about it)
    W[k][(st, p)] = n == nr or (nc > 0 and nc == ncr and not head)
del RR
del rows
J = {}
for l in open(JN, encoding="utf-8"):
    st = canon(l.strip().lstrip("*"))
    if st: J[key(st)] = st
VP = {}  # Vosk stress position per key (main stress = last "1")
VO = collections.defaultdict(set)  # key -> {(position, sound)} of the main stress
for l in open(VOSK, encoding="utf-8", errors="replace"):
    w, _, ph = l.rstrip("\n").partition("\t")
    k = 0; s = None; snd = None
    for p in ph.split():
        if p[-1:] in ("0", "1"):
            if p[-1] == "1": s = k; snd = p[0]
            k += 1
    if s is not None: VP.setdefault(key(w), set()).add(s); VO[key(w)].add((s, snd))
dropped = 0
for k, v in W.items():
    vo = VO.get(k)
    if not vo: continue
    for (f, p) in list(v):
        sp = spos(f)
        if sp < 0 or "ё" in f or [c for c in bare(f) if c in VOW][sp] != "е": continue
        if (sp, "o") in vo and (sp, "e") not in vo and any(spos(g) == sp and "ё" in g for g, _ in v):
            del v[(f, p)]; dropped += 1
print("dropped е́ where Vosk says ё", dropped)
DV = {}
for l in open(DIS, encoding="utf-8"):
    w, _, v = l.rstrip("\n").partition("  vosk: ")
    DV[w.strip()] = [x for x in (canon(x) for x in v.split(",")) if x]
U = json.load(open(VERD, encoding="utf-8"))
uv, um = U.get("v", {}), U.get("r", {})
user = collections.Counter()
for st, verdict in uv.items():
    c = canon(st); 
    if c is None: continue
    k = key(c)
    if verdict == "d":
        W[k] = {(c, False): False}; user["d"] += 1
    elif verdict == "v" and DV.get(st):
        W[k] = {(x, False): False for x in DV[st]}; user["v"] += 1
    elif verdict == "c":
        v = W[k]
        for x in [c] + DV.get(st, []):
            if not any(f == x for f, _ in v): v[(x, False)] = False
        user["c"] += 1
D = {}  # key -> (form, proper) default
for rk, m in um.items():
    w, _, rd = rk.partition("|")
    c = canon(rd)
    if c is None: continue
    k = key(c); p = rd[:1] != rd[:1].lower(); v = W[k]
    if (c, p) not in v: v[(c, p)] = False
    if m == "rare": v[(c, p)] = True
    elif m == "def": D[k] = (c, p)
    user["mark-" + m] += 1
stat = collections.Counter()
with open(OUT, "w", encoding="utf-8") as o:
    for k in sorted(W, key=lambda s: s.encode()):
        v = W[k]
        if not v: continue
        vp = VP.get(k, set())
        def order(it):
            (f, p), r = it
            return (D.get(k) != (f, p), r, -SC.get((k, f, p), 0.0), spos(f) not in vp, f != J.get(k), p, f)
        out = []
        for (f, p), r in sorted(v.items(), key=order):
            fl = ("n" if p else "") + ("r" if r else "") + ("d" if D.get(k) == (f, p) else "")
            out.append(f + ("/" + fl if fl else ""))
        if len({spos(f) for (f, p) in v}) > 1: stat["homographs"] += 1
        stat["words"] += 1
        o.write(k + "\t" + ",".join(out) + "\n")
with open(OUTJ, "w", encoding="utf-8") as o:
    for k in sorted(J, key=lambda s: s.encode()):
        if k not in W: o.write(k + "\t" + J[k] + "\n"); stat["jn"] += 1
print(dict(stat), dict(user))
