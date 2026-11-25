#!/usr/bin/python3 -I
"""norm-rn.py TTS-NORM-TEST: the c engine (rn) in tts-norm.c.

Needs the rn data (cmorphy3-ru.bin, en-ipa.tsv, made by textnorm/cmorphy3/mkdict.py
and textnorm/rn/gen-ipa.py) in $RN_DATA; skipped without it.  Checks a few
known answers, that garbage and long texts come back (normalized or as they
were) within a time limit, and that missing data falls back to the helper.
"""
import os
import random
import subprocess
import sys

TEST = sys.argv[1]
DATA = os.environ.get("RN_DATA", "")
if not (os.path.exists(os.path.join(DATA, "cmorphy3-ru.bin"))
        and os.path.exists(os.path.join(DATA, "en-ipa.tsv"))):
    print("no rn data in $RN_DATA; skipped")
    sys.exit(77)

KNOWN = [
    ("В 2024 г. цена выросла на 5 % до 300 руб.",
     "В две тысячи двадцать четвёртом году цена выросла на пять процентов до трёхсот рублей."),
    ("Встреча 12.03.2025 в 14:30, ул. Ленина д. 5.",
     "Встреча двенадцатого марта две тысячи двадцать пятого года в четырнадцать тридцать,"
     " улица Ленина дом пять."),
    ("Ок. 5 тыс. человек", "Около пяти тысяч человек"),
    ("XIV век, т.е. давно", "четырнадцатый век, то есть давно"),
    ("   ", "   "),
    # R3: a date that names the day is nominative; set off or followed by the clause: genitive
    ("Сегодня 23 ноября 1999 г.",
     "Сегодня двадцать третье ноября тысяча девятьсот девяносто девятого года."),
    ("Наступило 1 января. Сегодня, 23 ноября, идёт дождь. Это было 5 мая.",
     "Наступило первое января. Сегодня, двадцать третьего ноября, идёт дождь."
     " Это было пятого мая."),
    # rn-tech.c (R27–R30)
    ("Python 3.10 и Ubuntu 26.04 с GNOME 50, дом No 7, 10 / 2.",
     "пайтон три точка десять и убунту двадцать шесть точка ноль четыре с гном пятьдесят,"
     " дом номер семь, десять делить на два."),
    ("См. стр. 5, данные ООН на https://a.ru/b.",
     "смотри страницу пять, данные о+он на эй точка ру слэш би."),
    # R35: acronyms as one word, stress on the end; initials
    ("Сотрудники ГИБДД и МВД, в ТиНАО.", "Сотрудники гибэдэд+э и эмвэд+э, в тина+о."),
    ("Ю. Гагарин полетел. Роман написал Гашек Я.",
     "ю — Гагарин полетел. Роман написал Гашек, йа."),
    ("Цена 2.50 доллара, сумма 5 рублей, от потери 100 долларов.",
     "Цена два доллара пятьдесят центов, сумма пять рублей, от потери ста долларов."),
    # the user's stress marks survive, in their form (user-stress.c)
    ("В 2024 г. за\u0301мок построили",
     "В две тысячи двадцать четвёртом году за\u0301мок построили"),
    ("Ок. 5 тыс. зам+ков", "Около пяти тысяч зам+ков"),
    ("21 кни\u0301га", "двадцать одна кни\u0301га"),
    ("XIV ве\u0301к", "четырнадцатый ве\u0301к"),
    ("ЗА\u0301МОК 5 руб.", "За\u0301мок пять рублей."),   # rn's case, the mark kept
    ("к 5 дома\u0301м", "к пяти дома\u0301м"),
    ("2+она", "два +она"),    # a sum, not a mark (rn reads no "+")
]

rnd = random.Random(1)
GARBAGE = [
    "1." * 8000,
    "а" * 8000,
    "9" * 16000,
    " ".join("%d.%d.%d" % (rnd.randrange(40), rnd.randrange(14), rnd.randrange(3000))
             for _ in range(1500)),
    "".join(chr(rnd.choice([rnd.randrange(0x20, 0x7f), rnd.randrange(0x400, 0x460),
                            rnd.randrange(0x2000, 0x2070), rnd.randrange(0x1f300, 0x1f600)]))
            for _ in range(5000)),
    "http://" + "a." * 3000 + "ru/" + "x/" * 2000,
    "Hello WORLD it's a TEST, ok? " * 500,
    "т.е. т.к. и т.д. и т.п. г. ул. д. кв. руб. коп. " * 180,
    "\u0301\u0300" * 4000 + "5",
    "XIV MMXXIV IVX " * 900,
    "5 кг/м² 10 °C 3,14 −7 ½ № 5 § 3 " * 350,
]


def run(lines, *opts):
    inp = "".join(l.replace("\n", " ") + "\n" for l in lines)
    r = subprocess.run([TEST, *opts], input=inp.encode(), capture_output=True, timeout=120)
    out = r.stdout.decode().split("\n")[:-1]
    if r.returncode != 0 or len(out) != len(lines):
        sys.exit("FAIL: exit %d, %d lines for %d\n%s" % (r.returncode, len(out), len(lines),
                                                         r.stderr.decode()[-2000:]))
    return [tuple(o.split("\t", 1)) for o in out], r.stderr.decode()


fail = 0
got, _ = run([k for k, _ in KNOWN], "--data", DATA)
for (inp, want), (_, out) in zip(KNOWN, got):
    if out != want:
        print("FAIL: %r\n want %r\n got  %r" % (inp, want, out))
        fail = 1

got, err = run(GARBAGE, "--data", DATA)
for inp, (ms, out) in zip(GARBAGE, got):
    assert len(inp.encode()) <= 16384, "longer texts pass through"
    print("%6s ms  %5d -> %5d bytes  %r" % (ms, len(inp.encode()), len(out.encode()), inp[:30]))
    if int(ms) > 5000:
        print("FAIL: too slow")
        fail = 1
    if not out.strip():
        print("FAIL: lost the text")
        fail = 1
if err:
    print(err[-2000:])

# no data: the python engine (here a missing helper), the text unchanged
got, err = run(["5 кг"], "--data", "/nonexistent", "--helper", "/nonexistent")
if got[0][1] != "5 кг" or "no rn data" not in err:
    print("FAIL: no data: %r %r" % (got, err))
    fail = 1
# --engine python never touches rn
got, err = run(["5 кг"], "--engine", "python", "--data", DATA, "--helper", "/nonexistent")
if got[0][1] != "5 кг" or "rn" in err.split(":")[0:2]:
    print("FAIL: engine python: %r %r" % (got, err))
    fail = 1
sys.exit(fail)
