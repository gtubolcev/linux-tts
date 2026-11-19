# Wiktionary (kaikki en + ru extracts) -> stress dictionary
# out: bare<TAB>stressed<TAB>lemma<TAB>pos<TAB>tags<TAB>src, sorted by bytes
import gzip, json, sys, re, collections
D, OUT = sys.argv[1:3]
AC = "́"
VOW = set("аеёиоуыэюя")
CYR = re.compile(r"^[а-яё́̀-]+$")
SKIPT = {"romanization", "table-tags", "inflection-template", "class", "transliteration", "error-unknown-tag"}
def bare(s): return s.replace(AC, "").replace("̀", "")
def norm(s):
    s = s.lower().strip()
    s = s.replace("̀", "")
    return s
def ok(st):
    if not CYR.match(st): return False
    b = bare(st); nv = sum(c in VOW for c in b)
    if nv == 0: return False
    if nv == 1: return True
    return st.count(AC) >= 1 or "ё" in b
rows = collections.defaultdict(lambda: [set(), set()])  # key: (bare, stressed, lemma, pos, flags)  # (bare, stressed, lemma, pos) -> [tags, src]
def add(st, lemma, pos, tags, src, flags=""):
    st = norm(st)
    for part in st.split():  # multiword forms: skip
        pass
    if " " in st or not ok(st): return
    if st.count(AC) > 1: return  # variant marks "тво́ро́г": keep as two lines
    k = (bare(st), st, bare(norm(lemma)), pos, flags)
    r = rows[k]; r[0].update(t for t in tags if t not in SKIPT); r[1].add(src)
def split_multi(st):
    # "тво́ро́г" -> both variants
    i = [m.start() for m in re.finditer(AC, st)]
    if len(i) <= 1: return [st]
    b = st.replace(AC, ""); out = []
    pos = []; n = 0
    for c in st:
        if c == AC: pos.append(n)
        else: n += 1
    return [b[:p] + AC + b[p:] for p in pos]
RARE_EN = {"rare", "regional", "obsolete", "dialectal", "archaic", "historical", "nonstandard", "uncommon"}
RARE_RU = ("рег.", "устар.", "диал.", "редк.", "истор.", "простореч.", "обл.", "церк.", "старин.", "прост.")
RARE_FORM = {"dated", "obsolete", "archaic", "rare", "poetic", "dialectal", "regional", "nonstandard", "historical"}
def rare(d):
    ss = d.get("senses", [])
    if not ss: return False
    def r1(s):
        t = set(s.get("tags", [])) | set(s.get("raw_tags", []))
        if t & RARE_EN or any(x.startswith(RARE_RU) for x in t): return True
        g = " ".join(s.get("glosses", []) or s.get("raw_glosses", []))
        return g.startswith(RARE_RU) or g.startswith("(") and any(x in g[:40] for x in RARE_RU + tuple(RARE_EN))
    return all(r1(s) for s in ss)
def take(d, src):
    lemma = d.get("word", ""); pos = d.get("pos", "")
    flags = ("n" if lemma[:1].isupper() or pos == "name" else "") + ("r" if rare(d) else "")
    seen = False
    for f in d.get("forms", []):
        tags = f.get("tags", [])
        if set(tags) & {"romanization", "table-tags", "inflection-template", "transliteration"}: continue
        # an old or poetic form of a common word (руки́ gen. sg.): rare
        ff = flags + ("r" if "r" not in flags and set(tags) & RARE_FORM else "")
        for st in split_multi(norm(f.get("form", ""))):
            add(st, lemma, pos, tags, src, ff); seen = True
    for h in d.get("head_templates", []) or []:
        pass
    w = norm(lemma)
    if ok(w): add(w, lemma, pos, ["canonical"], src, flags)
n = 0
for l in open(f"{D}/en-ru.jsonl", encoding="utf-8"):
    d = json.loads(l)
    if d.get("lang_code", "ru") == "ru": take(d, "en"); n += 1
for l in gzip.open(f"{D}/ruwikt.jsonl.gz", "rt", encoding="utf-8"):
    if '"lang_code": "ru"' not in l: continue
    d = json.loads(l)
    if d.get("lang_code") == "ru": take(d, "ru"); n += 1
lines = []
for (b, st, lem, pos, fl), (tags, src) in rows.items():
    lines.append("\t".join([b, st, lem, pos, ",".join(sorted(tags)), "+".join(sorted(src)), fl]))
lines.sort(key=lambda s: s.encode())
with open(OUT, "w", encoding="utf-8") as o:
    o.write("\n".join(lines) + "\n")
words = collections.defaultdict(set)
for (b, st, lem, pos, fl) in rows: words[b].add(st)
print("entries", n, "lines", len(lines), "words", len(words), "homographs", sum(1 for v in words.values() if len(v) > 1))
