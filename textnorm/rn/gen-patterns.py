#!/usr/bin/python3 -I
"""gen-patterns.py RN_SRC CORPUS.jsonl OUT_DIR: every regex ru_normalizr compiles, in PCRE2.

Hooks re._compile, imports ru_normalizr and normalizes the corpus, so patterns built inside
functions are caught too. Each (pattern, flags) is recorded with its call site and, for
module-level patterns, its variable name. Writes:

  OUT_DIR/patterns.jsonl  {"id", "site", "names", "py", "flags", "groups", "groupindex",
                           "pcre", "calls"} per pattern, the catalogue for porting;
  OUT_DIR/rn-patterns.h   the PCRE2 sources as a C table (UTF-32 code units, PCRE2_UTF).

The translation goes through Python's own regex parser, not the pattern text: each
single-character item (literal, class, \\w, \\d, \\s, '.') is replaced by the exact set of
code points Python's engine matches for it under the active flags, found by scanning all
code points with that item compiled alone. So \\w, \\s, IGNORECASE and Unicode tables
behave as in the oracle's Python whatever PCRE2 version is used; \\b becomes look-arounds
over the exact \\w set.
"""
import json
import os
import re
import sys
import traceback

from re import _compiler, _constants as C, _parser as P

RN_SRC = sys.argv[1]
sys.modules["dawg"] = None
sys.path.insert(0, RN_SRC)

SEEN = {}            # (pattern, flags) -> record
ORDER = []
_orig_compile = re._compile


PHASE = ["import"]
SKIP = (os.sep + "re" + os.sep, "functools.py", "text_scan.py")


def site():
    """file:line of the innermost caller outside re; None if that is not ru_normalizr
    (stdlib or third-party modules compiling their own patterns)."""
    for fr in reversed(traceback.extract_stack()[:-2]):
        f = fr.filename
        if any(x in f for x in SKIP):
            continue
        if os.sep + "ru_normalizr" + os.sep in f:
            return "%s:%d" % (os.path.relpath(f, os.path.join(RN_SRC, "ru_normalizr")),
                              fr.lineno)
        return None
    return None


def hooked(pattern, flags):
    s = site() if isinstance(pattern, str) else None
    if s is not None:
        key = (pattern, int(flags))
        r = SEEN.get(key)
        if r is None:
            r = {"py": pattern, "flags": int(flags), "sites": {}, "calls": 0,
                 "phase": PHASE[0]}
            SEEN[key] = r
            ORDER.append(key)
        r["sites"][s] = r["sites"].get(s, 0) + 1
        r["calls"] += 1
    return _orig_compile(pattern, flags)


re._compile = hooked

import importlib  # noqa: E402
import pkgutil  # noqa: E402

import ru_normalizr  # noqa: E402
from ru_normalizr import NormalizeOptions, Normalizer  # noqa: E402

MODULES = {}
for mi in pkgutil.walk_packages(ru_normalizr.__path__, "ru_normalizr."):
    MODULES[mi.name] = importlib.import_module(mi.name)

norm = Normalizer(NormalizeOptions.tts())
PHASE[0] = "run"
with open(sys.argv[2], encoding="utf-8") as f:
    for line in f:
        try:
            norm.normalize(json.loads(line)["text"])
        except Exception:
            pass

# variable names of module-level patterns (also inside tuples, lists, dicts)
NAMES = {}


def note(obj, name, depth=0):
    pat = getattr(obj, "pattern", None)
    if isinstance(obj, re.Pattern) or (pat is not None and isinstance(pat, re.Pattern)):
        p = obj if isinstance(obj, re.Pattern) else pat
        NAMES.setdefault((p.pattern, int(p.flags) | re.UNICODE), []).append(name)
    elif depth < 2 and isinstance(obj, (tuple, list)):
        for i, x in enumerate(obj):
            note(x, "%s[%d]" % (name, i), depth + 1)
    elif depth < 2 and isinstance(obj, dict):
        for k, x in obj.items():
            note(x, "%s[%r]" % (name, k), depth + 1)


for mname, mod in MODULES.items():
    short = mname[len("ru_normalizr."):]
    for k, v in vars(mod).items():
        if getattr(v, "__module__", mname) != mname and isinstance(v, type):
            continue
        note(v, "%s.%s" % (short, k))
        if isinstance(v, type) and v.__module__ == mname:
            for k2, v2 in vars(v).items():
                note(v2, "%s.%s.%s" % (short, k, k2))

# --- translation -----------------------------------------------------------------------

ALL = "".join(chr(c) for c in range(0x110000) if not 0xD800 <= c <= 0xDFFF)
SETCACHE = {}
IGN, MUL, DOT, VERB, UNI, ASC = (re.IGNORECASE, re.MULTILINE, re.DOTALL, re.VERBOSE,
                                 re.UNICODE, re.ASCII)


def charset(node, flags):
    """Exact set of code points matched by one single-character parse item."""
    f = int(flags) & (IGN | DOT | ASC)
    key = (repr(node), f)
    s = SETCACHE.get(key)
    if s is None:
        sp = P.SubPattern(P.State(), [node])
        sp.state.flags = f | (0 if f & ASC else UNI)
        rx = _compiler.compile(sp, sp.state.flags)
        s = [m.start() for m in rx.finditer(ALL)]
        s = [ord(ALL[i]) for i in s]
        SETCACHE[key] = s
    return s


def cls(points, negate_ok=True):
    """PCRE2 class for a sorted list of code points."""
    if not points:
        return "(*FAIL)"
    rng = []
    a = b = points[0]
    for c in points[1:]:
        if c == b + 1:
            b = c
        else:
            rng.append((a, b))
            a = b = c
    rng.append((a, b))
    if len(rng) == 1 and rng[0][0] == rng[0][1]:
        return esc(rng[0][0])
    # a big set reads (and matches) better as its complement
    full = sum(b - a + 1 for a, b in rng)
    if negate_ok and full > 0x88000:
        comp = sorted(set(ord(c) for c in ALL) - set(points))
        return "[^" + "".join(r(a, b) for a, b in ranges(comp)) + "]"
    return "[" + "".join(r(a, b) for a, b in rng) + "]"


def ranges(points):
    out = []
    if not points:
        return out
    a = b = points[0]
    for c in points[1:]:
        if c == b + 1:
            b = c
        else:
            out.append((a, b))
            a = b = c
    out.append((a, b))
    return out


def esc(c):
    ch = chr(c)
    if ch.isascii() and (ch.isalnum() or ch == "_"):
        return ch
    if 0x20 < c < 0x7F:
        return "\\" + ch
    return "\\x{%x}" % c


def r(a, b):
    if a == b:
        return esc(a)
    if b == a + 1:
        return esc(a) + esc(b)
    return esc(a) + "-" + esc(b)


WORD = None


def word_class(flags):
    return "[A-Za-z0-9_]" if flags & ASC else "[\\p{L}\\p{N}_]"


SINGLE = {C.LITERAL, C.NOT_LITERAL, C.IN, C.ANY}


def tr(sp, flags, out):
    for op, av in sp:
        tr_item(op, av, flags, out)


def tr_seq(items, flags):
    out = []
    for op, av in items:
        tr_item(op, av, flags, out)
    return "".join(out)


# Python's \w, \d, \s as PCRE2 (no PCRE2_UCP: its \w took in marks in 10.43). Checked
# against Python's own sets below and, at build time, against PCRE2 by the charset test.
SPACE = "\\t\\n\\x{b}\\f\\r\\x{1c}-\\x{20}\\x{85}\\x{a0}\\x{1680}\\x{2000}-\\x{200a}\\x{2028}\\x{2029}\\x{202f}\\x{205f}\\x{3000}"
CAT_UNI = {C.CATEGORY_WORD: "\\p{L}\\p{N}_", C.CATEGORY_DIGIT: "\\p{Nd}",
           C.CATEGORY_SPACE: SPACE}
CAT_ASC = {C.CATEGORY_WORD: "A-Za-z0-9_", C.CATEGORY_DIGIT: "0-9",
           C.CATEGORY_SPACE: "\\t\\n\\x{b}\\f\\r\\x{20}"}
CAT_NEG = {C.CATEGORY_NOT_WORD: C.CATEGORY_WORD, C.CATEGORY_NOT_DIGIT: C.CATEGORY_DIGIT,
           C.CATEGORY_NOT_SPACE: C.CATEGORY_SPACE}


def in_class(items, flags):
    """PCRE2 for an IN item: literal members as an exact set, categories symbolically."""
    neg = bool(items) and items[0][0] == C.NEGATE
    if neg:
        items = items[1:]
    lits = [x for x in items if x[0] != C.CATEGORY]
    cats = [x[1] for x in items if x[0] == C.CATEGORY]
    table = CAT_ASC if flags & ASC else CAT_UNI
    pos = "".join(table[c] for c in cats if c in table)
    negs = [table[CAT_NEG[c]] for c in cats if c in CAT_NEG]
    body = ""
    if lits:
        pts = charset((C.IN, lits), flags)
        body = "".join(r(a, b) for a, b in ranges(pts))
    if not cats:
        return cls(charset((C.IN, ([(C.NEGATE, None)] if neg else []) + lits), flags))
    if not negs:
        return ("[^" if neg else "[") + body + pos + "]"
    if not neg and not body and not pos and len(negs) == 1:
        return "[^" + negs[0] + "]"
    if neg and len(negs) == 1:
        # not (body | pos | not N) = N and not (body | pos)
        # grouped: a quantifier after it must repeat the look-ahead too
        if not (body or pos):
            return "[" + negs[0] + "]"
        return "(?:(?![" + body + pos + "])[" + negs[0] + "])"
    if neg:
        raise NotImplementedError("negated class with negated categories")
    alts = (["[" + body + pos + "]"] if body or pos else []) + ["[^" + n + "]" for n in negs]
    return "(?:" + "|".join(alts) + ")"


def tr_item(op, av, flags, out):
    if op in SINGLE:
        if op == C.ANY and flags & DOT:
            out.append("[\\x{0}-\\x{10ffff}]")
        elif op == C.ANY:
            out.append("[^\\n]")
        elif op == C.IN:
            out.append(in_class(av, flags))
        else:
            out.append(cls(charset((op, av), flags)))
    elif op == C.SUBPATTERN:
        gid, add, dele, p = av
        f2 = (flags | add) & ~dele
        inner = tr_seq(p, f2)
        name = NAMEOF.get(gid) if gid is not None else None
        out.append(("(?<%s>" % name if name else "(" if gid is not None else "(?:") + inner + ")")
    elif op == C.BRANCH:
        out.append("(?:" + "|".join(tr_seq(b, flags) for b in av[1]) + ")")
    elif op in (C.MAX_REPEAT, C.MIN_REPEAT, C.POSSESSIVE_REPEAT):
        lo, hi, item = av
        inner = tr_seq(item, flags)
        if not (len(item) == 1 and item[0][0] in SINGLE):
            inner = "(?:" + inner + ")"
        if (lo, hi) == (0, C.MAXREPEAT):
            q = "*"
        elif (lo, hi) == (1, C.MAXREPEAT):
            q = "+"
        elif (lo, hi) == (0, 1):
            q = "?"
        elif hi == C.MAXREPEAT:
            q = "{%d,}" % lo
        elif lo == hi:
            q = "{%d}" % lo
        else:
            q = "{%d,%d}" % (lo, hi)
        q += {C.MAX_REPEAT: "", C.MIN_REPEAT: "?", C.POSSESSIVE_REPEAT: "+"}[op]
        out.append(inner + q)
    elif op == C.ATOMIC_GROUP:
        out.append("(?>" + tr_seq(av, flags) + ")")
    elif op in (C.ASSERT, C.ASSERT_NOT):
        d, p = av
        kind = {(C.ASSERT, 1): "(?=", (C.ASSERT, -1): "(?<=", (C.ASSERT_NOT, 1): "(?!",
                (C.ASSERT_NOT, -1): "(?<!"}[(op, d)]
        out.append(kind + tr_seq(p, flags) + ")")
    elif op == C.GROUPREF:
        if flags & IGN:
            raise NotImplementedError("case-insensitive backreference")
        out.append("\\g{%d}" % av)
    elif op == C.GROUPREF_EXISTS:
        gid, yes, no = av
        out.append("(?(%d)%s%s)" % (gid, tr_seq(yes, flags),
                                    "|" + tr_seq(no, flags) if no else ""))
    elif op == C.AT:
        w = word_class(flags)
        out.append({
            C.AT_BEGINNING: "(?m:^)" if flags & MUL else "\\A",
            C.AT_BEGINNING_STRING: "\\A",
            C.AT_END: "(?m:$)" if flags & MUL else "(?=\\n?\\z)",
            C.AT_END_STRING: "\\z",
            C.AT_BOUNDARY: "(?:(?<=%s)(?!%s)|(?<!%s)(?=%s))" % (w, w, w, w),
            C.AT_NON_BOUNDARY: "(?:(?<=%s)(?=%s)|(?<!%s)(?!%s))" % (w, w, w, w),
        }[av])
    else:
        raise NotImplementedError(str(op))


NAMEOF = {}


def translate(pattern, flags):
    sp = P.parse(pattern, flags)
    NAMEOF.clear()
    NAMEOF.update({v: k for k, v in sp.state.groupdict.items()})
    out = []
    tr(sp, sp.state.flags, out)
    return "".join(out)


def cstr(s):
    """C string literal of a PCRE2 pattern (pure ASCII after translation)."""
    assert s.isascii(), s
    return '"' + s.replace("\\", "\\\\").replace('"', '\\"') + '"'


os.makedirs(sys.argv[3], exist_ok=True)

# Python's \w, \d, \s against the symbolic forms used above (same Python tables)
import unicodedata  # noqa: E402

for cat, test in ((C.CATEGORY_WORD, lambda c: unicodedata.category(c)[0] in "LN" or c == "_"),
                  (C.CATEGORY_DIGIT, lambda c: unicodedata.category(c) == "Nd")):
    py = set(charset((C.IN, [(C.CATEGORY, cat)]), 0))
    uc = set(ord(c) for c in ALL if test(c))
    if py != uc:
        print("WARNING %s: Python differs from Unicode categories at %s" %
              (cat, sorted(py ^ uc)[:20]), file=sys.stderr)
sp = set(charset((C.IN, [(C.CATEGORY, C.CATEGORY_SPACE)]), 0))
pysp = re.sub(r"\\x\{([0-9a-f]+)\}", lambda m: "\\U%08x" % int(m.group(1), 16), SPACE)
mine = set(charset((C.IN, P.parse("[" + pysp + "]")[0][1]), 0))
assert sp == mine, sorted(sp ^ mine)
with open(os.path.join(sys.argv[3], "py-charsets.txt"), "w") as f:
    for name, cat in (("w", C.CATEGORY_WORD), ("d", C.CATEGORY_DIGIT), ("s", C.CATEGORY_SPACE)):
        pts = charset((C.IN, [(C.CATEGORY, cat)]), 0)
        f.write("%s %s\n" % (name, " ".join("%x-%x" % ab for ab in ranges(pts))))
# Python's \w and \d as explicit ranges: rn_re_compile() puts them in place of \p{L}\p{N}_
# and \p{Nd}, so the sets are the oracle's Unicode version, not PCRE2's
with open(os.path.join(sys.argv[3], "rn-unidata.h"), "w") as f:
    f.write("/* generated by gen-patterns.py from Python %s (Unicode %s) */\n"
            % (sys.version.split()[0], unicodedata.unidata_version))
    for name, cat in (("RN_UNI_WORD", C.CATEGORY_WORD), ("RN_UNI_DIGIT", C.CATEGORY_DIGIT)):
        pts = charset((C.IN, [(C.CATEGORY, cat)]), 0)
        body = "".join(r(a, b) for a, b in ranges(pts))
        f.write("#define %s \\\n" % name)
        for i in range(0, len(body), 90):
            f.write('\t"%s"%s\n' % (body[i:i + 90].replace("\\", "\\\\"),
                                     " \\" if i + 90 < len(body) else ""))

# dynamic sites: patterns built at run time (several per site); the C code builds those
SITE_PATS = {}
for key in ORDER:
    for st in SEEN[key]["sites"]:
        SITE_PATS.setdefault(st, set()).add(key)
DYNAMIC = {st for st, ks in SITE_PATS.items()
           if len(ks) > 1 and any(SEEN[k]["phase"] == "run" for k in ks)}


def cname(rec, used):
    if rec["names"]:
        n = rec["names"][0]
    else:
        n = rec["site"].replace(".py:", "_L")
    n = re.sub(r"[^A-Za-z0-9]+", "_", n).strip("_").upper()
    base, k = "RN_" + n, 2
    n = base
    while n in used:
        n = "%s_%d" % (base, k)
        k += 1
    used.add(n)
    return n


recs = []
bad = 0
used = set()
for i, key in enumerate(ORDER):
    r0 = SEEN[key]
    rx = re.compile(*key)
    sites = sorted(r0["sites"].items(), key=lambda x: -x[1])
    dyn = any(st in DYNAMIC for st, _ in sites)
    try:
        pcre = translate(*key)
    except NotImplementedError as e:
        pcre = None
        bad += 1
        print("untranslated #%d %s: %r" % (i, e, key[0][:80]), file=sys.stderr)
    rec = {"id": i, "site": sites[0][0], "sites": [st for st, _ in sites],
           "names": NAMES.get((key[0], key[1] | re.UNICODE), []), "phase": r0["phase"], "dynamic": dyn,
           "py": key[0], "flags": key[1], "groups": rx.groups,
           "groupindex": dict(rx.groupindex), "pcre": pcre, "calls": r0["calls"]}
    rec["cname"] = None if dyn else cname(rec, used)
    recs.append(rec)
with open(os.path.join(sys.argv[3], "patterns.jsonl"), "w", encoding="utf-8") as f:
    for rec in recs:
        f.write(json.dumps(rec, ensure_ascii=False) + "\n")
static = [rec for rec in recs if not rec["dynamic"]]
with open(os.path.join(sys.argv[3], "rn-patterns.h"), "w", encoding="ascii") as f:
    f.write("/* generated by gen-patterns.py from ru_normalizr; do not edit.\n"
            " * Patterns built at run time (dynamic sites) are not here. */\n")
    f.write("enum rn_pattern {\n")
    for rec in static:
        f.write("\t%s,\n" % rec["cname"])
    f.write("\tRN_NPATTERNS\n};\n")
    f.write("#ifdef RN_PATTERNS_DEFINE\n")
    f.write("static const char *const RN_PCRE[RN_NPATTERNS] = {\n")
    for rec in static:
        f.write("\t/* %s %s */\n\t[%s] = %s,\n" % (
            rec["site"], ascii(rec["py"][:70]).replace("*/", "* /").replace("/*", "/ *"), rec["cname"],
            cstr(rec["pcre"]) if rec["pcre"] else "NULL"))
    f.write("};\n#endif\n")
print("%d patterns: %d static, %d from %d dynamic sites (%s); %d untranslated" %
      (len(recs), len(static), len(recs) - len(static), len(DYNAMIC), " ".join(sorted(DYNAMIC)),
       bad), file=sys.stderr)
