#!/usr/bin/python3 -I
"""pyn2w-dump.py < numbers > ref.txt: num2words(lang='ru') over all option combos, one line each.

Same order as n2w-dump: for each number, cardinal then ordinal; case n g d a i p; gender m f n;
plural 0 1; animate 1 0. Floats as float(), ints as int(); '!' where num2words raises."""
import sys

from num2words import num2words

CASES = ["nominative", "genitive", "dative", "accusative", "instrumental", "prepositional"]
CG = ["masculine", "feminine", "neuter"]
OG = ["m", "f", "n"]
w = sys.stdout.write
for line in sys.stdin:
    s = line.strip()
    num = float(s) if "." in s else int(s)
    for to, genders in (("cardinal", CG), ("ordinal", OG)):
        for c in CASES:
            for g in genders:
                for pl in (False, True):
                    for an in (True, False):
                        try:
                            res = num2words(num, lang="ru", to=to, case=c, gender=g,
                                            plural=pl, animate=an)
                        except Exception:
                            res = "!"
                        w(res + "\n")
