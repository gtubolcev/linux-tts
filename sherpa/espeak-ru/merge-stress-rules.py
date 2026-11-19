#!/usr/bin/python3 -I
# merge-stress-rules.py RU_RULES STRESS_RULES OUTPUT
# puts each group of STRESS_RULES at the top of the same .group of RU_RULES
import sys
src, add, dst = sys.argv[1:4]
groups, cur = {}, None
for line in open(add, encoding="utf-8"):
    if line.startswith(".group"):
        cur = line.split()[1]
        groups[cur] = []
    elif cur and line.strip() and not line.startswith("//"):
        groups[cur].append(line)
out = []
for line in open(src, encoding="utf-8"):
    out.append(line)
    w = line.split()
    if line.startswith(".group") and len(w) == 2 and w[1] in groups:
        out += groups.pop(w[1])
if groups:
    sys.exit("no .group in %s for: %s" % (src, " ".join(groups)))
with open(dst, "w", encoding="utf-8") as f:
    f.writelines(out)
