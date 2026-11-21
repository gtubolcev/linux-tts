"""Reference for phone-check.c (run in the oracle's Python).

python3 -I phone-check.py gen > numbers   # test inputs: digit strings after "+"
python3 -I phone-check.py < numbers > ref.txt
"""
import random
import sys

import phonenumbers
from phonenumbers import PhoneNumberFormat, PhoneNumberType
from phonenumbers.phonenumberutil import COUNTRY_CODE_TO_REGION_CODE


def gen():
    rnd = random.Random(20261121)
    out = set()
    for cc, regions in COUNTRY_CODE_TO_REGION_CODE.items():
        c = str(cc)
        examples = []
        for region in regions:
            for t in range(12):
                try:
                    if region == "001":
                        ex = phonenumbers.example_number_for_non_geo_entity(cc)
                    else:
                        ex = phonenumbers.example_number_for_type(region, t)
                except Exception:
                    ex = None
                if ex is not None:
                    examples.append(phonenumbers.national_significant_number(ex))
        for nsn in examples:
            for v in (nsn, "0" + nsn, "8" + nsn, "1" + nsn, "9" + nsn, nsn[:-1], nsn + "5",
                      nsn[:-2], nsn + "12", "00" + nsn, nsn[1:]):
                out.add(c + v)
            for _ in range(20):
                k = rnd.randrange(len(nsn))
                out.add(c + nsn[:k] + str(rnd.randrange(10)) + nsn[k + 1:])
        for n in range(1, 15 - len(c) + 1):
            for _ in range(30):
                out.add(c + "".join(str(rnd.randrange(10)) for _ in range(n)))
    for _ in range(50000):
        n = rnd.randrange(1, 16)
        out.add(str(rnd.randrange(1, 10)) + "".join(str(rnd.randrange(10)) for _ in range(n - 1)))
    for d in sorted(out):
        if 1 <= len(d) <= 15:
            print(d)


def check(d):
    try:
        number = phonenumbers.parse("+" + d, None)
    except phonenumbers.NumberParseException:
        return "NO"
    if not phonenumbers.is_valid_number(number):
        return "NO"
    if phonenumbers.format_number(number, PhoneNumberFormat.E164) != "+" + d:
        return "NO"
    import re
    f = phonenumbers.format_number(number, PhoneNumberFormat.INTERNATIONAL)
    return "OK " + " ".join(re.findall(r"[0-9]+", f))


if len(sys.argv) > 1 and sys.argv[1] == "gen":
    gen()
else:
    for line in sys.stdin:
        d = line.strip()
        print(d + "\t" + check(d))
