#!/usr/bin/python3 -I
"""mkdict.py DICT_DIR OUT.bin: convert pymorphy3-dicts-ru data to one file for cmorphy3.

DICT_DIR is the pymorphy3 dictionary directory (meta.json, grammemes.json,
gramtab-opencorpora-int.json, suffixes.json, paradigms.array, words.dawg,
prediction-suffixes-N.dawg, p_t_given_w.intdawg). The output is little-endian and read with
mmap by cmorphy3.c; the DAWG files are copied unchanged.

Layout: header "CMORPHY3", u32 version, u32 section count, then per section u32 id, u32
reserved, u64 offset, u64 length; sections are 8-byte aligned. Strings live in the STR
section as u16 byte length, u16 character length, UTF-8 bytes, NUL; other sections refer to
them by u32 offset.
"""
import json
import os
import struct
import sys

SRC, OUT = sys.argv[1], sys.argv[2]
VERSION = 1


def jload(name):
    with open(os.path.join(SRC, name), encoding="utf-8") as f:
        return json.load(f)


meta = dict(jload("meta.json"))
if not str(meta.get("format_version", "")).startswith("2."):
    sys.exit("unsupported dictionary format %r" % meta.get("format_version"))
if meta.get("language_code") != "ru":
    sys.exit("only the Russian dictionary is supported")

# grammemes: the dictionary's, then those pymorphy3's analyzer units add at run time
grammemes = jload("grammemes.json")
names = [g[0] for g in grammemes]
for extra in ["UNKN", "PNCT", "LATN", "NUMB", "intg", "real", "ROMN", "Init", "Name", "Patr"]:
    if extra not in names:
        grammemes.append([extra, "", extra, ""])
        names.append(extra)
if len(names) > 128:
    sys.exit("more than 128 grammemes")
gidx = {n: i for i, n in enumerate(names)}

# OpencorporaTag._init_grammemes: a grammeme is incompatible with its siblings (same
# parent); 'plur' is also incompatible with 'GNdr' and its children
parent = {g[0]: g[1] for g in grammemes}
children = {}
for g in grammemes[:len(jload("grammemes.json"))]:
    n, p = g[0], g[1]
    if p:
        children.setdefault(p, set()).add(n)
    if parent.get(p):
        children.setdefault(parent[p], set()).add(n)
extra_incompat = {"plur": {"GNdr"} | children.get("GNdr", set())}
incompat = [(extra_incompat.get(n, set()) | children.get(parent[n], set())) - {n} for n in names]


def bits(gset):
    v = 0
    for g in gset:
        v |= 1 << gidx[g]
    return v


strings = bytearray()
str_index = {}


def sref(s):
    if s in str_index:
        return str_index[s]
    b = s.encode("utf-8")
    off = len(strings)
    strings.extend(struct.pack("<HH", len(b), len(s)))
    strings.extend(b + b"\0")
    while len(strings) % 4:
        strings.append(0)
    str_index[s] = off
    return off


# tags: the gramtab, then the tags analyzer units create
gramtab = jload("gramtab-opencorpora-int.json")
INIT_FIRST = "NOUN,anim,%s,Sgtm,Name,Fixd,Abbr,Init sing,%s"
INIT_PATR = "NOUN,anim,%s,Sgtm,Patr,Fixd,Abbr,Init sing,%s"
CASES6 = ["nomn", "gent", "datv", "accs", "ablt", "loct"]
special = ["UNKN", "PNCT", "LATN", "NUMB,intg", "NUMB,real", "ROMN", "ADVB"]
special += [INIT_FIRST % (g, c) for g in ("masc", "femn") for c in CASES6]
special += [INIT_PATR % (g, c) for g in ("masc", "femn") for c in CASES6]
tags = list(gramtab)
tag_id = {t: i for i, t in enumerate(tags)}  # str(tag) is the key of P(t|w) and the output
special_ids = []
for t in special:
    if t not in tag_id:
        tag_id[t] = len(tags)
        tags.append(t)
    special_ids.append(tag_id[t])
# tags compare by their grammeme tuple ('COMP Infr' == 'COMP,Infr'): eq = the first tag id
# with the same tuple
first = {}
tag_eq = [first.setdefault(tuple(t.replace(" ", ",", 1).split(",")), i) for i, t in enumerate(tags)]
if len(tags) >= 65535:
    sys.exit("too many tags")

sec = {}

# 1 GRAM: u32 n, then n × (u32 name, u32 pad, u64 incompat lo, u64 incompat hi)
b = bytearray(struct.pack("<I", len(names)))
b.extend(b"\0" * 4)
for n, inc in zip(names, incompat):
    v = bits(inc)
    b.extend(struct.pack("<IIQQ", sref(n), 0, v & (2**64 - 1), v >> 64))
sec[1] = bytes(b)

# 2 TAGS: u32 n, u32 pad, then n × (u64 lo, u64 hi, u32 str, u16 first grammeme, u16 eq)
b = bytearray(struct.pack("<II", len(tags), 0))
for t, eq in zip(tags, tag_eq):
    gl = t.replace(" ", ",", 1).split(",")
    v = bits(gl)
    b.extend(struct.pack("<QQIHH", v & (2**64 - 1), v >> 64, sref(t), gidx[gl[0]], eq))
sec[2] = bytes(b)

# 3 SPECIAL: u32 n, then the tag ids of `special`, in order
sec[3] = struct.pack("<I%dI" % len(special_ids), len(special_ids), *special_ids)

# 4 SUFFIXES: u32 n, then string offsets
suffixes = jload("suffixes.json")
sec[4] = struct.pack("<I%dI" % len(suffixes), len(suffixes), *[sref(s) for s in suffixes])

# 5 PREFIXES (paradigm prefixes)
prefixes = meta["compile_options"]["paradigm_prefixes"]
sec[5] = struct.pack("<I%dI" % len(prefixes), len(prefixes), *[sref(s) for s in prefixes])

# 6 PARADIGMS: u32 n, u32 start[n+1] (in u16 units), u16 data
with open(os.path.join(SRC, "paradigms.array"), "rb") as f:
    data = f.read()
count = struct.unpack_from("<H", data, 0)[0]
pos, starts, units = 2, [0], []
for _ in range(count):
    ln = struct.unpack_from("<H", data, pos)[0]
    pos += 2
    para = struct.unpack_from("<%dH" % ln, data, pos)
    pos += 2 * ln
    if ln % 3:
        sys.exit("bad paradigm length")
    n3 = ln // 3
    for k in range(n3):
        if para[k] >= len(suffixes) or para[n3 + k] >= len(gramtab) or para[2 * n3 + k] >= len(prefixes):
            sys.exit("paradigm refers past a table")
    units.extend(para)
    starts.append(len(units))
if pos != len(data):
    sys.exit("trailing bytes in paradigms.array")
sec[6] = struct.pack("<I%dI" % len(starts), count, *starts) + struct.pack("<%dH" % len(units), *units)

# 7 WORDS, 8..10 PRED0..2, 11 PTW: DAWG files as they are
with open(os.path.join(SRC, "words.dawg"), "rb") as f:
    sec[7] = f.read()
for i in range(len(prefixes)):
    with open(os.path.join(SRC, "prediction-suffixes-%d.dawg" % i), "rb") as f:
        sec[8 + i] = f.read()
with open(os.path.join(SRC, "p_t_given_w.intdawg"), "rb") as f:
    sec[11] = f.read()

# 12 META: u32 max_suffix_length, then the source revision string offset
sec[12] = struct.pack("<II", meta["compile_options"]["max_suffix_length"],
                      sref("%s %s" % (meta.get("source_revision"), meta.get("corpus_revision"))))

sec[0] = bytes(strings)  # STR last, after every sref()

ids = sorted(sec)
hdr_len = 16 + 24 * len(ids)
out = bytearray()
table = []
off = (hdr_len + 7) & ~7
body = bytearray()
for i in ids:
    while (off + len(body)) % 8:
        body.append(0)
    table.append((i, off + len(body), len(sec[i])))
    body.extend(sec[i])
out.extend(b"CMORPHY3" + struct.pack("<II", VERSION, len(ids)))
for i, o, ln in table:
    out.extend(struct.pack("<IIQQ", i, 0, o, ln))
while len(out) < off:
    out.append(0)
out.extend(body)
tmp = OUT + ".tmp"
with open(tmp, "wb") as f:
    f.write(out)
os.replace(tmp, OUT)
print("%s: %d bytes, %d grammemes, %d tags, %d paradigms" % (OUT, len(out), len(names), len(tags), count),
      file=sys.stderr)
