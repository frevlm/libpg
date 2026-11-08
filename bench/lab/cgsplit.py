"""Splits a callgrind dump of the bench's measured window (lab.sh cgwin) per operation.

    cgsplit.py SCENARIO.opsN.window

Prints instructions per operation: the total, the self cost by object (the
library, libpq, libc, ...), a tree of inclusive costs of the library's stages,
and calls per operation of the functions that count work (round trips,
statements sent, allocations). Inclusive costs come from callgrind's per-call
costs, so a stage is exact; "rest" under a stage is what its listed children
do not cover.
"""
import re, sys
from collections import defaultdict

path = sys.argv[1]
ops = int(re.search(r"\.ops(\d+)\.window$", path).group(1))

names = {"fn": {}, "cfn": {}, "ob": {}, "cob": {}}
self_cost = defaultdict(int)
edge = defaultdict(int)       # (caller, callee) -> inclusive cost of those calls
calls = defaultdict(int)      # callee -> calls
obj_of = {}
fn = ob = cfn = cob = None
pending_call = None


def resolve(kind, rest):
    m = re.match(r"\((\d+)\)(?: (.*))?$", rest)
    if not m:
        return rest
    table = names["fn" if kind in ("fn", "cfn") else "ob"]
    if m.group(2) is not None:
        table[m.group(1)] = m.group(2)
    return table[m.group(1)]


for line in open(path, errors="replace"):
    line = line.rstrip("\n")
    if not line or line[0] == "#":
        continue
    if "=" in line and re.match(r"^[a-z]+=", line):
        key, rest = line.split("=", 1)
        if key == "fn":
            fn = resolve("fn", rest)
            obj_of.setdefault(fn, ob)
        elif key == "cfn":
            cfn = resolve("cfn", rest)
        elif key == "ob":
            ob = resolve("ob", rest)
        elif key == "cob":
            cob = resolve("ob", rest)
        elif key == "calls":
            pending_call = int(rest.split()[0])
        continue
    if line[0] not in "0123456789+-*":
        continue
    parts = line.split()
    if len(parts) < 2:
        continue
    cost = int(parts[-1])
    if pending_call is not None:
        if cfn != fn:
            edge[(fn, cfn)] += cost
        calls[cfn] += pending_call
        obj_of.setdefault(cfn, cob or ob)
        pending_call = None
        cob = None
    else:
        self_cost[fn] += cost

funcs = set(self_cost) | {c for (_, c) in edge}
incl = defaultdict(int)
for f in funcs:
    incl[f] += self_cost.get(f, 0)
for (f, c), v in edge.items():
    incl[f] += v
total = sum(self_cost.values())


def short(name):
    name = re.sub(r"std::__cxx11::basic_string<char, std::char_traits<char>, std::allocator<char> >", "string", name)
    name = name.replace("(anonymous namespace)::", "")
    name = re.sub(r"\(.*", "", name)
    return name[:90]


def per(v):
    return v / ops


def match(rx):
    """Inclusive cost and calls of the functions whose name matches; nested matches counted once."""
    hit = [f for f in funcs if re.search(rx, f)]
    inside = set(hit)
    cost = sum(incl[f] for f in hit) - sum(v for (a, b), v in edge.items() if a in inside and b in inside)
    return cost, sum(calls[f] for f in hit)


print(f"## {path.split('/')[-1]}: {ops} operations, {per(total):,.0f} instructions per operation\n")

print("### self cost by object\n")
by_obj = defaultdict(int)
for f, v in self_cost.items():
    by_obj[(obj_of.get(f) or "?").split("/")[-1]] += v
for o, v in sorted(by_obj.items(), key=lambda x: -x[1])[:8]:
    print(f"- {o}: {per(v):,.0f} ({100 * v / total:.1f}%)")

# The library's stages, as (label, regex, children). Each regex names the entry
# function(s) of a stage; a child is entered only from inside its parent.
TREE = [
    ("operation (bench loop)", r"^bench::Ctx::loop|_Function_handler<void \(bench::Ctx&\), bench::\(anonymous namespace\)::run_handler", [
        ("conn: acquire", r"^pg::conn::conn\(", []),
        ("conn: release (scope end, learning)", r"^pg::conn::~conn\(", [
            ("learn (observe + table)", r"pg::detail::\(anonymous namespace\)::learn\(", [
                ("observe (parameter sources)", r"pg::detail::\(anonymous namespace\)::observe\(", []),
                ("rebuild plan", r"pg::detail::\(anonymous namespace\)::rebuild\(", []),
            ]),
            ("scope clear (frees results)", r"^pg::detail::prefetch_scope::clear\(", []),
        ]),
        ("statement building (pg::sql)", r"^pg::sql::sql<|^pg::sql::operator\+=|^pg::sql::append", []),
        ("conn::run", r"^pg::conn::run\(", [
            ("render", r"^pg::sql::render\(", []),
            ("prefetched (scope bookkeeping + plan)", r"^pg::conn::prefetched\(", [
                ("table look-up", r"look_up\(", []),
                ("cache find", r"find_cached\(", []),
                ("cache serve", r"serve_cached\(|view_of\(", []),
                ("record in trace", r"pg::detail::\(anonymous namespace\)::record\(|record_held\(", []),
                ("run_plan", r"^pg::conn::run_plan\(", [
                    ("send batch", r"^pg::detail::speculate\(", [
                        ("libpq: build + send", r"^PQsendQueryPrepared$|^PQsendPrepare$|^PQpipelineSync$|^PQflush$", []),
                    ]),
                    ("wait + read batch", r"^pg::detail::land_speculation\(", [
                        ("libpq: parse", r"^PQconsumeInput$|^PQisBusy$|^PQgetResult$", []),
                    ]),
                    ("slice into windows", r"absorb\(|slice_unified\(|slice_members\(|carve\(", []),
                ]),
                ("unplanned read (send + wait + read)", r"^pg::conn::run_unscoped\(", []),
            ]),
            ("plain read (send + wait + read)", r"^pg::detail::run_floating\(", [
                ("send", r"^pg::detail::execute_begin\(", [
                    ("libpq: build + send", r"^PQsendQueryPrepared$|^PQpipelineSync$|^PQflush$", []),
                ]),
                ("wait + read", r"^pg::detail::execute_end\(", [
                    ("libpq: parse", r"^PQconsumeInput$|^PQisBusy$|^PQgetResult$", []),
                ]),
            ]),
        ]),
    ]),
]
# Under "operation", rest is the bench's own code and row decoding, which the
# compiler inlines into the query calls.


def show(nodes, depth):
    for label, rx, kids in nodes:
        cost, n = match(rx)
        if cost == 0 and n == 0:
            continue
        times = f", {n / ops:.2f} calls/op" if n >= ops else ""
        print(f"{'  ' * depth}- {label}: {per(cost):,.0f} ({100 * cost / total:.1f}%){times}")
        if kids:
            covered = sum(match(k[1])[0] for k in kids)
            show(kids, depth + 1)
            if cost - covered > 0.02 * cost:
                print(f"{'  ' * (depth + 1)}- rest: {per(cost - covered):,.0f}")


print("\n### stages (inclusive)\n")
show(TREE, 0)

print("\n### calls per operation\n")
for label, rx in [
    ("round trips waited (execute_end)", r"^pg::detail::execute_end\("),
    ("statements sent (PQsendQueryPrepared)", r"^PQsendQueryPrepared$"),
    ("results read (PQgetResult)", r"^PQgetResult$"),
    ("malloc", r"^malloc$"),
    ("free", r"^free$"),
    ("operator new", r"^operator new\("),
    ("pg::sql::render", r"^pg::sql::render\("),
]:
    cost, n = match(rx)
    print(f"- {label}: {n / ops:.2f} calls, {per(cost):,.0f} instructions")

print("\n### top inclusive functions of the library\n")
lib = [f for f in funcs if f.startswith("pg::") and not f.startswith("pg::codec")]
for f in sorted(lib, key=lambda f: -incl[f])[:25]:
    print(f"- {per(incl[f]):>9,.0f}  {calls[f] / ops:6.2f}/op  {short(f)}")

print("\n### top self cost (every object)\n")
for f in sorted(self_cost, key=lambda f: -self_cost[f])[:30]:
    o = (obj_of.get(f) or "?").split("/")[-1]
    print(f"- {per(self_cost[f]):>8,.0f}  {100 * self_cost[f] / total:4.1f}%  {o[:16]:16s}  {short(f)}")
