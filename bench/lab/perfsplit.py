"""Splits server CPU per operation by PostgreSQL stage, from `perf report --children` (lab.sh perf).

    perfsplit.py REPORT.txt SERVER_US_PER_OP

REPORT.txt is `perf report --stdio --children --sort sym -g none` of the lab
server's cgroup while the bench saturates it. Each stage's inclusive share of
the samples times the server CPU per operation (from the bench's cgroup
accounting) gives microseconds per operation. Stages are entry functions of
the backend's message loop; nested ones are listed under their parent.
"""
import re, sys

report, srv_us = sys.argv[1], float(sys.argv[2])
children, self_pct, kernel = {}, {}, set()
for line in open(report, errors="replace"):
    m = re.match(r"\s*([\d.]+)%\s+([\d.]+)%\s+(?:\S+\s+)?\[([.k])\]\s+(.+?)\s*$", line)
    if m:
        name = m.group(4)
        children[name] = children.get(name, 0) + float(m.group(1))
        self_pct[name] = self_pct.get(name, 0) + float(m.group(2))
        if m.group(3) == "k":
            kernel.add(name)


def pct(names):
    return max((children.get(n, 0) for n in names), default=0)


# Stages by exported functions (the protocol handlers in postgres.c are static
# and have no symbol): reading the client's messages, Bind's plan look-up and
# executor start, Execute's run, the portal's drop, the implicit transaction
# around each Sync group, and writing results back.
TREE = [
    ("backend message loop", ["PostgresMain"], [
        ("read from client (secure_read)", ["secure_read"], []),
        ("Bind: plan look-up (GetCachedPlan)", ["GetCachedPlan"], []),
        ("Bind: executor start (PortalStart)", ["PortalStart"], []),
        ("Execute (PortalRun)", ["PortalRun"], [
            ("executor run", ["standard_ExecutorRun"], []),
        ]),
        ("executor end + portal drop (PortalDrop)", ["PortalDrop"], []),
        ("transaction start (StartTransactionCommand)", ["StartTransactionCommand"], []),
        ("transaction end at Sync (CommitTransactionCommand)", ["CommitTransactionCommand"], []),
        ("write to client (secure_write)", ["secure_write"], []),
    ]),
]


def show(nodes, depth):
    for label, names, kids in nodes:
        p = pct(names)
        if p == 0:
            continue
        print(f"{'  ' * depth}- {label}: {p * srv_us / 100:.1f} µs/op ({p:.1f}%)")
        if kids:
            show(kids, depth + 1)
            rest = p - sum(pct(k[1]) for k in kids)
            if rest > 0.05 * p:
                print(f"{'  ' * (depth + 1)}- rest: {rest * srv_us / 100:.1f} µs/op ({rest:.1f}%)")


print(f"server {srv_us:.1f} µs/op; samples of the server cgroup, inclusive\n")
show(TREE, 0)
total_main = pct(["PostgresMain"])
if total_main < 99:
    print(f"- outside the message loop (other processes, unwinding gaps): {100 - total_main:.1f}%")
print(f"\nkernel (self time in kernel functions): {sum(self_pct[k] for k in kernel) * srv_us / 100:.1f} µs/op "
      f"({sum(self_pct[k] for k in kernel):.1f}%)")
print("\ntop self functions:")
for k, v in sorted(self_pct.items(), key=lambda x: -x[1])[:18]:
    print(f"- {v:5.1f}%  {v * srv_us / 100:6.1f} µs/op  {k[:80]}")
