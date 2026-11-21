#!/usr/bin/python3 -I
"""gen-numbers.py > numbers.txt: test numbers for n2w-ru (ints, then floats as str(float))."""
import random

r = random.Random(1)
nums = set(range(0, 3001))
for k in range(1, 34):
    p = 10 ** k
    for m in (1, 2, 3, 5, 9, 11, 21, 99, 100, 101, 111, 999):
        for v in (m * p, m * p + 1, m * p - 1, m * p + 1000, m * p + 21000, m * p + 1000000):
            if 0 < v < 10 ** 33:
                nums.add(v)
for _ in range(20000):
    nums.add(r.randrange(10 ** r.randint(1, 33)))
for _ in range(3000):   # chunks with zeros: 1 000 001, 21 000 000 ...
    d = r.randint(2, 11)
    nums.add(int("".join(r.choice(["000", "001", "021", "100", "111", "200", "011", "090", "999"])
                         for _ in range(d))))
out = [str(n) for n in sorted(nums)]
out += ["-" + str(n) for n in sorted(r.sample(sorted(nums), 2000))]
fl = set()
for _ in range(3000):
    fl.add(str(round(r.uniform(-1000, 100000), r.randint(1, 6))))
for s in ("0.5", "-0.5", "1.1", "1.11", "1.01", "2.21", "11.5", "-1.5", "-21.5", "0.0", "1.0",
          "100.25", "1.123456789", "21.000001", "3.14159", "1001.1", "-0.01"):
    fl.add(s)
out += sorted(fl)
print("\n".join(out))
