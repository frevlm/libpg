"""Summarizes interleaved A/B runs: lab CSV rows are variant,rep,scenario,threads,pool,ops,p50,p99,p999,extra."""
import csv, statistics, sys
from collections import defaultdict

rows = defaultdict(list)
order = []
for r in csv.reader(open(sys.argv[1])):
    if len(r) < 9 or r[0] == "variant":
        continue
    key = (r[2], r[3], r[4])
    if key not in order:
        order.append(key)
    rows[key, r[0]].append((float(r[5]), float(r[6]), float(r[7]), float(r[8])))
a, b = sys.argv[2], sys.argv[3]
print(f"| scenario | thr | pool | {a} ops/s (min..max) | {b} ops/s (min..max) | {b}/{a} | p50 µs {a}→{b} | p99 µs {a}→{b} | p999 µs {a}→{b} | verdict |")
print("|---|---|---|---|---|---|---|---|---|---|")
for k in order:
    A, B = rows.get((k, a)), rows.get((k, b))
    if not A or not B:
        continue
    oa, ob = [x[0] for x in A], [x[0] for x in B]
    ma, mb = statistics.median(oa), statistics.median(ob)
    verdict = f"{b} faster" if min(ob) > max(oa) else f"{a} faster" if min(oa) > max(ob) else "within noise"
    med = lambda xs, i: statistics.median(x[i] for x in xs)
    print(f"| {k[0]} | {k[1]} | {k[2]} | {ma:.0f} ({min(oa):.0f}..{max(oa):.0f}) | {mb:.0f} ({min(ob):.0f}..{max(ob):.0f}) "
          f"| {mb / ma:.3f} | {med(A, 1):.0f}→{med(B, 1):.0f} | {med(A, 2):.0f}→{med(B, 2):.0f} | {med(A, 3):.0f}→{med(B, 3):.0f} | {verdict} |")
