"""Summarizes `lab.sh run`: lab CSV rows are variant,rep,scenario,threads,pool,ops,p50,p99,p999,extra.

CPU per operation (cli_us, srv_us in extra, from bench/harness.cpp) is the median over reps."""
import csv, statistics, sys
from collections import defaultdict

rows = defaultdict(list)
cpu = defaultdict(lambda: defaultdict(list))
extra = {}
order = []
for r in csv.reader(open(sys.argv[1])):
    if len(r) < 9 or r[0] == "variant" or not r[6]:
        continue
    key = (r[2], r[3], r[4])
    if key not in order:
        order.append(key)
    rows[key].append((float(r[5]), float(r[6]), float(r[7]), float(r[8])))
    rest = []
    for kv in (r[9] if len(r) > 9 else "").split(";"):
        k, _, v = kv.partition("=")
        if k in ("cli_us", "srv_us"):
            cpu[key][k].append(float(v))
        elif kv and k != "ops":
            rest.append(kv)
    extra[key] = ";".join(rest)


def cpu_cell(key, k):
    xs = cpu[key][k]
    return f"{statistics.median(xs):.1f}" if xs else ""


print("| scenario | thr | pool | ops/s (min..max) | p50 µs | p99 µs | p999 µs | client CPU µs/op | server CPU µs/op | extra |")
print("|---|---|---|---|---|---|---|---|---|---|")
for k in sorted(order, key=lambda k: (int(k[1]), order.index(k))):
    xs = rows[k]
    ops = [x[0] for x in xs]
    med = lambda i: statistics.median(x[i] for x in xs)
    print(f"| {k[0]} | {k[1]} | {k[2]} | {statistics.median(ops):.0f} ({min(ops):.0f}..{max(ops):.0f}) "
          f"| {med(1):.0f} | {med(2):.0f} | {med(3):.0f} | {cpu_cell(k, 'cli_us')} | {cpu_cell(k, 'srv_us')} | {extra[k]} |")
