# Bench lab

Remote A/B lab for proving that a performance mechanism pays for itself. Each
mechanism is removed by a patch in `ablate/`, built next to the unpatched tree,
and both builds are run in alternating order against a dedicated PostgreSQL on the
lab host. When a mechanism shows no gain, its patch is the start of the commit that
deletes it.

```
bench/lab/lab.sh setup
bench/lab/lab.sh build base
bench/lab/lab.sh build no-load bench/lab/ablate/no-load.patch
DELAY_US=500 THREADS="4 16" bench/lab/lab.sh ab base no-load v2_get_row_slow
bench/lab/lab.sh cpu base no-load v2_get_row:PQsendQueryPrepared
```

## Resolution

Measured by comparing two identical builds (4 reps × 3 s) on a 2 vCPU host that
also runs production services:

| measurement | resolution |
|---|---|
| wall clock, 1 thread, `DELAY_US=500` | ±2–3% |
| wall clock, 1 thread, no delay | ±5–10% |
| wall clock, 16 threads | ±20–30% |
| callgrind marginal instructions per call (`cpu`) | ±1% |

Use these as follows:

- Round-trip mechanisms: judge them at 1 thread with `DELAY_US=500`.
- CPU mechanisms: judge them with `cpu`.
- In-process contention: use `BENCH_CPUS=0-1`. On one core, a spin lock is always in its worst case.
- Tail-latency mechanisms: judge them on p999.

## Evidence (2026-11-07)

| mechanism | removed by | result | decision |
|---|---|---|---|
| spin-then-sleep pool waiter | spin budget 0 | 16 threads, pool 4, delays 0 and 500 µs: throughput and p99 within noise | removed |
| load estimate (`load`, slow-session skip) | `ablate/no-load.patch` | a 50 ms statement next to 15 readers: p999 goes from 6–8 ms to 50 ms, in 5 of 5 reps, with or without delay | kept |
| floating statements | `ablate/no-float.patch` | single-statement throughput is within noise. `*_sent` scenarios deadlock: 16 threads each hold one lease and wait for a second, with pool 8 | kept: `conn::send` depends on it |
| writeback `spin_mutex` | `ablate/wb-mutex.patch` (std::mutex) | `wb_get_add_hot` is 10–13% slower with std::mutex at 1 and 16 threads (2 CPUs) | kept |
| 64 slot-table shards | `ablate/wb-1shard.patch` | `wb_get_add` at 16 threads: −33% throughput, p99 2 → 315 µs | kept |
| prepared statement cache | raw libpq scenarios | 16 threads, no delay: unnamed text 7.8k/s vs prepared 15.9k/s, because the server parses and plans each unnamed statement. 1 thread at 500 µs: +8% | kept |

## Fleet (several pods at once)

`lab.sh fleet NAME PODS direct|bouncer SCENARIO...` runs PODS bench processes at
once, each with a pool of its own. They connect straight to the server or through
PgBouncer 1.26 in transaction mode with 16 server connections. Measured with
8 threads and pool 8 per pod, delay 500 µs, `max_connections=100`:

| pods | direct | through PgBouncer |
|---|---|---|
| 4 | get_row 12.0k/s, p999 26 ms | 6.6k/s, p999 16 ms |
| 10 | get_row 10.7k/s, p999 1.3 s | 7.2k/s, p999 44 ms |
| 24 | 14 of 24 pods fail to connect | all 24 run, 6.9k/s, p999 83 ms |

On this 2 vCPU host PgBouncer shares a core with the server, so the throughput
ratio reflects the host, not production. What carries over is the shape of the
result:

- Direct connections fail outright past `max_connections`.
- Through the pooler, every pod runs and the tail stays bounded.

## Bugs found by the lab

- A transaction holding an unread `conn::send` deadlocked its commit whenever
  another borrower was waiting for the connection: 2 threads and pool 1 were enough.
  The reader handed the reading to the front ticket even when nobody waited on it,
  which happens to an unread sent statement. Fixed: the reading now goes to the
  oldest ticket a thread sleeps on. Test: "transactions with unread sent statements
  finish while others wait for the connection".

## Real read shapes (2026-11-07)

`bench/scn_real.cpp` replays read paths of HHHOJ and revlm, each in the form the application
uses today and in forms with fewer statements or fewer columns. Data: `bench/schema_real.sql`
(`lab.sh load`), compared with `lab.sh run` (4 reps × 3 s, pool 8). Median ops/s:

| handler | today | best form | 1 thread, delay 500 µs | 1 thread, no delay | 16 threads, no delay |
|---|---|---|---|---|---|
| HHHOJ `/api/auth/me` (8 statements) | `real_me_serial` | `real_me_one` (1 statement) | 158 → 702 | 682 → 1193 | 828 → 1229 |
| same, independent reads only pipelined | `real_me_serial` | `real_me_sent` (3 round trips) | 158 → 327 | 682 → 744 | 828 → 848 (noise) |
| HHHOJ submission list, N+1 (12 statements) | `real_list_n1` | `real_list_join` (2 statements, 1 round trip) | 128 → 1291 | 888 → 4032 | 1530 → 5562 |
| HHHOJ judger idle poll (6 statements) | `real_poll_serial` | `real_poll_sent` (1 round trip) | 257 → 1094 | 2077 → 2866 | 3826 → 5065 |
| mix 60/30/10 of the above | `real_mix_serial` | `real_mix_best` | 157 → 850 | 764 → 1612 | 990 → 1632 |

- Fewer statements wins at every load. Pipelining alone wins latency when the round trip is long,
  and gives nothing once the server CPU is the limit (`real_me_sent` at 16 threads).
- `real_me_*` at 16 threads is capped by `count(*) FROM oj_users WHERE usergroup = 1` (9800 index
  tuples per request), which no client mechanism removes.

revlm dashboard over `rv_requests` (4M rows, 1073 MB, user rows scattered over the heap;
`shared_buffers` 128 MB), 1 thread:

| query | no covering index | with `(user_id, time) INCLUDE (input_tokens, output_tokens, usd)` |
|---|---|---|
| last day, all 16 columns (`real_dash_day_rows`) | 101 ops/s | 508 ops/s |
| last day, 4 columns (`real_dash_day_proj`) | 117 ops/s | 2294 ops/s |
| last day, GROUP BY hour on the server (`real_dash_day_agg`) | 120 ops/s | 2736 ops/s |
| whole history, all columns (`real_dash_all_rows`, 40k rows) | 5.2 s | 3.3 s |
| whole history, 4 columns (`real_dash_all_proj`) | 3.3 s | 31 ms |
| whole history, GROUP BY hour (`real_dash_all_agg`) | 3.2 s | 25 ms |

- Without the index, every form reads one heap page per row (39711 buffer reads for 40000 rows):
  projection and server aggregation gain under 40%.
- Reading only the needed columns is what lets an index-only scan answer: 100× on the whole history.
  A read of every column can never use it.
- The lab database now has `rv_requests_user_cover`; drop it to reproduce the left column.

## Learned prefetch (ADR 0025, 2026-11-07)

`real_*_auto` run the bodies of `real_*_serial` / `real_list_n1` on a pool with `prefetch = true`; the code is
not changed. Build `ro` (batches on read-only sessions), one run with all forms, 3 reps × 3 s, pool 8 (prefetch adds
up to 2 read-only connections), median ops/s; fail=0 in every run, skip ≤ 32 of ~10^5 scopes:

| handler | serial → auto, 1 thread, 500 µs | 16 threads, 500 µs | 1 thread, no delay | 16 threads, no delay | hand-written best, 500 µs (1 / 16 threads) |
|---|---|---|---|---|---|
| auth/me | 163 → 425 | 703 → 958 | 487 → 717 | 788 → 967 | 713 / 1211 (`real_me_one`) |
| submission list, N+1 | 126 → 1210 | 1285 → 4760 | 710 → 3627 | 1423 → 4921 | 1252 / 5763 (`real_list_join`) |
| judger idle poll | 256 → 1364 | 3154 → 8189 | 1528 → 5103 | 3495 → 8301 | 1076 / 6455 (`real_poll_sent`) |
| mix 60/30/10 | 149 → 556 | 868 → 1350 | 699 → 1145 | 974 → 1405 | 823 / 1480 (`real_mix_best`) |

The no-delay run had a disturbed host (me_serial 272..579, poll_serial 865..1593 at 1 thread); read its 1-thread
column as direction only.

Guards against read-only sessions (`lab.sh ab`, 3 reps × 3 s, same host session):

| handler, 16 threads | rev 4 (BEGIN READ ONLY + timeout + ROLLBACK per batch) | guards dropped, unsafe (`ng`) | read-only sessions (`ro`) |
|---|---|---|---|
| list, 500 µs / 0 | 3778 / 4082 | 4190 / 4673 | 4970 / 4950 |
| poll, 500 µs / 0 | 6568 / 5556 | 7491 / 7965 | 10869 / 8020 |
| mix, 500 µs / 0 | 1093 / 1187 | 1213 / 1256 | 1413 / 1397 |

`ng` and `ro` were each A/B'd against rev 4 in their own run; rev 4 shown from the `ng` run. Dropping the three
guard statements alone is +10–15%; read-only sessions gain more, since speculative batches no longer queue on the
sessions that carry the program's own statements (and the pool has up to 2 more connections).

Client cost (callgrind marginal instructions, per handler = per `~conn`; point reads per statement):

| measurement | off (serial) | auto, rev 4 | auto, `ro` | auto, `pr` | auto, `f2` |
|---|---|---|---|---|---|
| point read with nothing to learn (`real_point_*`) | 11956 | 12490 | 12496 | 12496 | 12519 (+4.7%) |
| auth/me | 102k | — | 191k | 123k | 95k |
| submission list, N+1 | 167k | — | 189k | 190k | 134k |
| judger idle poll | 62k | — | 66k | 66k | 47k |
| mix 60/30/10 (varies ±8% run to run with the draw) | 113–120k | 187k | 179k | 129–141k | 103k |

- `pr`: a follower that is not in the plan no longer marks the plan changed when it fades out. auth/me's card read
  binds a value with no source, so it entered the table as a new literal follower every scope and faded 8 scopes
  later; each fade rebuilt and recompiled the whole plan (635 rebuilds in 1107 mix scopes, 18% of all instructions).
- `f2`: follower results are windows (first row, rows, first column) over the batch's PGresult instead of copies
  (`slice_of` was 19% of list_auto), and the scope cache matches a 64-bit hash of text and params, then the exact
  text and params, from a flat vector (sorted index above 64 entries) instead of building a string key per read.
- With prefetch on, every handler now costs the client fewer instructions than its serial form: fewer statements
  through libpq. The lab server has its own core, so this moved 16-thread throughput by only +2–8% (`f2` vs `ro`,
  no delay); it matters where the client and the database share CPUs.

- auth/me keeps one miss per scope by design: the card read binds `'{' || name || '}'`, a value with no source.
- The first version re-executed the root inside every composite and wrapped each speculative statement in its own
  transaction; at 16 threads that made the judger poll slower than serial (5909 → 3419). The unified form (parent
  served from the composite) and one read-only batch per scope fixed both.

## Where the ceiling is (2026-11-07, build `f3`)

Tools: every `run`/`ab` row now carries client and server CPU per operation over the measured window
(`getrusage` of the bench process, the server container's cgroup `cpu.stat`); `lab.sh cgwin` is callgrind of the
window only, split by `cgsplit.py`; `lab.sh pgss` sums `pg_stat_statements` over the window; `lab.sh perf` samples
the server's cgroup with perf while 16 threads saturate it and splits it by backend stage (`perfsplit.py`).

**16 threads: the server's CPU per operation is the ceiling.** The server has one core; every scenario runs at
95–99% of 10^6 / server µs per operation (delay 0):

| handler | form | server µs/op | bound ops/s | measured | client µs/op |
|---|---|---|---|---|---|
| auth/me | serial / auto / one statement | 1203 / 1001 / 784 | 831 / 999 / 1276 | 813 / 981 / 1263 | 381 / 186 / 74 |
| list | N+1 / auto / join | 632 / 198 / 180 | 1582 / 5051 / 5556 | 1512 / 4895 / 5308 | 467 / 101 / 98 |
| poll | serial / auto / pipelined | 252 / 110 / 190 | 3963 / 9074 / 5263 | 3609 / 8623 / 4949 | 218 / 68 / 164 |
| mix | serial / auto / best | 961 / 678 / 605 | 1041 / 1474 / 1652 | 1003 / 1454 / 1623 | 388 / 158 / 99 |

**Server CPU model** (perf, by backend stage; within ~10% for simple statements, composites cost more per Sync
group: larger results to write, more relations locked at commit):

    server µs/op ≈ 30 × Sync groups + ~7.5 × statements (more for a composite) + executor run

- Per Sync group ≈ 30 µs: writing the results (`secure_write`, ≈15 µs, mostly kernel TCP over the docker veth and
  conntrack), reading the messages (≈6 µs), the implicit transaction's commit (≈4 µs, more with more relations
  locked), the rest of the loop (≈4 µs). Batched groups on a shared session also share reads.
- Per statement ≈ 7.5 µs for a simple one: plan look-up (≈1), executor start (≈4.5), executor end and portal drop
  (≈1.5). A composite's executor start grows with its plan: poll's 5-member composite 34 µs, me's 6 members 122 µs.
- Executor run is the SQL's own work.

| scenario | executor run | executor start + end + plan | per-Sync (read + write + commit + loop) | total |
|---|---|---|---|---|
| point read | 6.4 | 6.7 | 30 | 43 |
| poll serial (6 Syncs) | 22 | 44 | 184 | 252 |
| poll auto (1) | 27 | 44 | 31 | 107 |
| list N+1 (12) | 128 | 121 | 395 | 650 |
| list auto (1) | 93 | 36 | 46 | 180 |
| list join | 69 | 36 | 73 | 178 |
| me one statement | 674 | 74 | 64 | 832 |
| me auto (2) | 745 | 196 | 127 | 1087 |

- list and poll: prefetch already sits at or below the hand-written form's server cost. What remains is executor
  work and per-statement start/end, which fusing does not remove (a composite pays for each member's plan nodes).
- auth/me: 62–81% of the server is one statement of the application, `count(*) FROM oj_users WHERE usergroup = 1`
  (≈600–700 µs). No client-side mechanism moves this handler past ≈1300 ops/s per server core. Prefetch's own gap to
  the one-statement form: the unsourced card read's second round trip, a composite executor start of 122 µs (vs 57),
  and 56 µs/op in `GetCachedPlan` (custom plans; not yet tried: `plan_cache_mode` on the read-only sessions).

**1 thread: round trips × (delay + ≈70 µs) + client + server.** At 500 µs the model predicts every auto and
hand-written form within 1–4% (me_auto 2 round trips: 2404 µs predicted, 2427 measured; list_join's two statements
travel in one round trip). The floor for any read handler is one round trip plus its CPU.

**Client** (callgrind of the window, instructions per operation, 1 thread): list_auto 134k vs list_join 60k.
Allocation is the largest reducible part of the auto form: 137 malloc + 138 operator new + 141 free per operation
(≈30k instructions, 23%) against 36 + 25 + 42 for the join. libpq's parsing is ≈23k for both. Prefetch's
bookkeeping: observe 10k, scope clear 7k, record 5k, slicing 5k, `run_plan`'s own code ≈6k. The client CPU in µs
(user + kernel) is equal for list auto and join (93 vs 95): one round trip fewer pays for the bookkeeping, since
each round trip costs the client ≈25–30 µs of kernel time as well.

## PostgreSQL 19 (2026-11-19, libpg 0.4.0)

Setup: a second server, `LAB_PG=libpg-lab-pg19 LAB_PG_IMAGE=postgres:19 lab.sh setup`, with the same flags and
its own volume. Both servers were reseeded from `schema.sql` and `schema_real.sql`, then `VACUUM ANALYZE` and
restarted. A server that has been up for weeks has its buffers in swap: its point reads ran 5.8× slower before
the restart. libpq 19 was unpacked with `dpkg -x` into `/lab/pq19` and selected per build (`LAB_PQ`).

Variants: `s18` is server 18 with libpq 18.7, `s19` is server 19 with libpq 18.7, `s19q19` is server 19 with
libpq 19.0. Ctest passes 20/20 ×3 against server 19 with both libpqs. `pg_tablegen` output is unchanged.

| question | method | result |
|---|---|---|
| client: libpq 19 vs 18 | `cgwin`, instructions per operation | identical: get_row 17,112 → 17,108, list_50 261,045 → 261,507, tx3 42,183 → 42,173. libpq 19 adds only OAuth v2 and `PQgetThreadLock` |
| server: point read, list, tx3, COPY, real handlers | `ab s18 s19` (6 reps, 1 and 16 threads, delay 0 and 500 µs) and null `ab s19 s19n` | within noise: srv_us 51.8 → 51.6 (get_row), 20.8 → 20.4 (wb_requests), 545 → 544 (mix_best) |
| server: `v2_list_50` at 1 thread, which the first run called faster (229 → 209) | repeated on freshly restarted servers, 8 reps | 208 → 210, within noise |
| server: a 44k-row filtered index scan, warm cache (`real_dash_day_agg`, `_proj`) | `ab`, 8 reps, 1 thread; `_proj` repeated without `all_agg` in the batch | srv_us −13%: 7785 → 6788 (agg) and 7609 → 6559 (proj), the ranges do not overlap; control `v2_get_row` 49.8 → 49.9. Same plan; the projection form gains as much as the aggregate form, so the gain is in the scan rather than the aggregation |
| server: a 40k-page cold bitmap heap scan (`real_dash_all_agg`) | `EXPLAIN ANALYZE` ×3 | 5.2–6.0 s → 1.8–3.1 s. The cause is a default: 19 scales IO workers 2..8 where 18 has `io_workers=3`. 19 capped at 3 workers: 5.0–6.0 s. 18 with `io_workers=8`: 2.8–3.1 s |

Noise floor of srv_us (null A/B, server 19 against itself): medians of 1-thread runs differ by up to 0.80–1.26×
(list_join, get_row), because a few reps are slow. Judge on the range overlap and on the minimum.

Default settings that differ and can matter: `jit` off (18: on; no lab query reaches `jit_above_cost`),
`default_toast_compression=lz4`, `max_locks_per_transaction=128`, `enable_eager_aggregate=on`, IO workers
scaled 2..8.
