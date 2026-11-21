#!/usr/bin/python3 -I
"""Python's side of pystr-check.c."""
import sys

w = sys.stdout.write
for cp in range(0x110000):
    if 0xD800 <= cp < 0xE000:
        continue
    c = chr(cp)
    flags = "".join("1" if f else "0" for f in (
        c.isalpha(), c.isdecimal(), c.isdigit(), c.isnumeric(), c.isalnum(), c.isupper(),
        c.islower(), c.isspace(), c.isupper()))
    hx = lambda s: "".join(" %X" % ord(x) for x in s)
    w("%X %s%s |%s |%s\n" % (cp, flags, hx(c.lower()), hx(c.upper()), hx(c.casefold())))
for s in ("AΣ", "1Σ", "AΣB", "A\xadΣ", "Σ", "AΣ\xad"):
    w("ctx%s\n" % "".join(" %X" % ord(x) for x in s.lower()))
