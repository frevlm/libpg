#!/bin/bash
# Runs the libpg benchmarks on a throwaway PostgreSQL cluster.
#
#   bench/run.sh
#
# Environment (defaults in brackets):
#   DURATION    seconds measured per point [5]; every point also has a 1 s warmup
#   THREADS     thread counts for most scenarios and for pgbench ["1 2 4 8 16 32 64"]
#   POOL        pool sizes for the pooled v1 scenarios ["8 16 32"]
#   PC_THREADS  pool_contention thread counts ["8 16 32 64 128"]
#   PC_POOL     pool_contention pool sizes ["4 8 16 32"]
#   SCENARIOS   space separated pg_bench scenario names [all]; see `pg_bench --list`
#   SKIP_PGBENCH=1  skip the pgbench reference runs
#   SOCKET=1    connect over a unix socket instead of 127.0.0.1 TCP [0]
#   PORT [54400], PGBIN [/opt/homebrew/bin], BUILD_DIR [<repo>/build]
# Results: bench/results/<timestamp>.csv, .md and .log
set -eu

here=$(cd "$(dirname "$0")" && pwd)
root=$(dirname "$here")

DURATION=${DURATION:-5}
THREADS=${THREADS:-"1 2 4 8 16 32 64"}
POOL=${POOL:-"8 16 32"}
PC_THREADS=${PC_THREADS:-"8 16 32 64 128"}
PC_POOL=${PC_POOL:-"4 8 16 32"}
SCENARIOS=${SCENARIOS:-}
SKIP_PGBENCH=${SKIP_PGBENCH:-0}
SOCKET=${SOCKET:-0}
PORT=${PORT:-54400}
PGBIN=${PGBIN:-/opt/homebrew/bin}
BUILD_DIR=${BUILD_DIR:-$root/build}
# On macOS the postmaster refuses to start without a valid LC_ALL.
export LC_ALL=C

data=$(mktemp -d "${TMPDIR:-/tmp}/libpg-bench.XXXXXX")
sockdir=
if [ "$SOCKET" = 1 ]; then
  # Unix socket paths are short on macOS (104 bytes), keep this one under /tmp.
  sockdir=$(mktemp -d /tmp/libpg-sock.XXXXXX)
fi

stop_cluster() {
  if [ -f "$data/postmaster.pid" ]; then
    "$PGBIN/pg_ctl" -D "$data" -m immediate -w stop >/dev/null 2>&1 || true
  fi
  rm -rf "$data"
  [ -z "$sockdir" ] || rm -rf "$sockdir"
}
trap stop_cluster EXIT
trap 'exit 130' INT TERM

if [ "$SOCKET" = 1 ]; then
  host=$sockdir
  dsn="host=$sockdir port=$PORT user=postgres dbname=bench"
else
  host=127.0.0.1
  dsn="postgresql://postgres@127.0.0.1:$PORT/bench"
fi

# --- build -----------------------------------------------------------------------
if [ ! -x "$BUILD_DIR/bench/pg_bench" ]; then
  cmake -S "$root" -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE=Release -DLIBPG_BENCH=ON >/dev/null
fi
cmake --build "$BUILD_DIR" -j --target pg_bench >/dev/null
pg_bench=$BUILD_DIR/bench/pg_bench

# --- cluster ---------------------------------------------------------------------
"$PGBIN/initdb" -D "$data" -U postgres -A trust -E UTF8 --no-locale --no-sync >/dev/null
"$PGBIN/pg_ctl" -D "$data" -l "$data/server.log" -w \
  -o "-p $PORT -c listen_addresses=127.0.0.1 -c unix_socket_directories=$sockdir \
-c max_connections=300 -c shared_buffers=512MB -c fsync=off -c synchronous_commit=off \
-c full_page_writes=off -c max_wal_size=8GB -c checkpoint_timeout=30min" start >/dev/null ||
  { cat "$data/server.log" >&2; exit 1; }
psql="$PGBIN/psql -h $host -p $PORT -U postgres -X -q -v ON_ERROR_STOP=1"
"$PGBIN/createdb" -h "$host" -p "$PORT" -U postgres bench
"$PGBIN/createdb" -h "$host" -p "$PORT" -U postgres pgb
$psql -d bench -f "$here/schema.sql"
"$PGBIN/pgbench" -h "$host" -p "$PORT" -U postgres -i -s 10 -q pgb >/dev/null 2>&1

# --- outputs ---------------------------------------------------------------------
mkdir -p "$here/results"
stamp=$(date +%Y%m%d-%H%M%S)
csv=$here/results/$stamp.csv
md=$here/results/$stamp.md
log=$here/results/$stamp.log
echo "scenario,threads,pool,ops_per_sec,p50_us,p99_us,p999_us,extra" >"$csv"

# --- pgbench reference -----------------------------------------------------------
# pgbench -S for one protocol mode and client count; appends one CSV line.
pgbench_point() {
  mode=$1 clients=$2
  jobs=$clients
  [ "$jobs" -le 10 ] || jobs=10
  prefix=$data/pgb-$mode-$clients
  out=$("$PGBIN/pgbench" -h "$host" -p "$PORT" -U postgres -S -M "$mode" -c "$clients" -j "$jobs" \
    -T "$DURATION" -l --log-prefix="$prefix" pgb)
  tps=$(printf '%s\n' "$out" | sed -n 's/^tps = \([0-9.]*\).*/\1/p')
  avg=$(printf '%s\n' "$out" | sed -n 's/^latency average = \([0-9.]*\) ms.*/\1/p')
  # Per-transaction log: column 3 is the latency in microseconds.
  pct=$(cat "$prefix".* | cut -d' ' -f3 | sort -n | awk '
    { a[NR] = $1 }
    END { printf "%.3f,%.3f,%.3f", a[int(NR * 0.50) + 1], a[int(NR * 0.99) + 1], a[int(NR * 0.999) + 1] }')
  rm -f "$prefix".*
  tps=$(awk -v t="$tps" 'BEGIN { printf "%.1f", t }')
  echo "pgbench_S_$mode,$clients,0,$tps,$pct,lat_avg_ms=$avg" >>"$csv"
  echo "pgbench -S -M $mode -c $clients: $tps tps" >&2
}

run_pgbench() {
  for mode in simple extended prepared; do
    for t in $THREADS; do
      pgbench_point "$mode" "$t"
    done
  done
}

# --- report header ---------------------------------------------------------------
{
  echo "# libpg benchmark $stamp"
  echo
  echo "- machine: $(sysctl -n machdep.cpu.brand_string 2>/dev/null || uname -m), $(sysctl -n hw.ncpu 2>/dev/null || echo ?) cores, $(uname -sr)"
  echo "- server: $("$PGBIN/postgres" --version), 127.0.0.1 TCP$([ "$SOCKET" = 1 ] && echo ' (clients on unix socket)') port $PORT"
  echo "- server settings: max_connections=300 shared_buffers=512MB fsync=off synchronous_commit=off full_page_writes=off"
  echo "- DURATION=${DURATION}s (+1s warmup) THREADS=\"$THREADS\" POOL=\"$POOL\" PC_THREADS=\"$PC_THREADS\" PC_POOL=\"$PC_POOL\""
  echo "- schema: bench/schema.sql, 100k accounts, uniform random keys; pgbench -S on scale 10 (database pgb)"
  echo "- latency columns of pg_bench rows are per operation (per batch for raw_pipeline_*; per transaction for *_tx3);"
  echo "  pgbench percentiles come from its per-transaction logs"
  echo
} >"$md"

# --- run -------------------------------------------------------------------------
status=0
if [ "$SKIP_PGBENCH" != 1 ]; then
  run_pgbench 2>>"$log" || status=1
fi
# shellcheck disable=SC2086
"$pg_bench" --dsn "$dsn" --duration "$DURATION" --threads "$THREADS" --pool "$POOL" \
  --pc-threads "$PC_THREADS" --pc-pool "$PC_POOL" $SCENARIOS >>"$csv" 2> >(tee -a "$log" >&2) || status=1

"$pg_bench" --summarize "$csv" >>"$md"
echo "results: $csv" >&2
echo "report:  $md" >&2
exit $status
