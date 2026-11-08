#!/bin/bash
# Remote benchmark lab: a dedicated PostgreSQL and bench containers on the lab host,
# so that variants of libpg compare on the same server, interleaved, with optional
# injected network delay. Runs from the dev machine; everything else is over ssh.
#
#   lab.sh setup                    build the image, start the lab server, seed it
#   lab.sh build NAME [PATCH...]    copy the working tree, apply patches, build pg_bench
#   lab.sh load FILE.sql            run a SQL file against the lab database
#   lab.sh ab A B SCENARIO...       interleaved wall-clock runs of variants A and B
#   lab.sh run NAME SCENARIO...     interleaved wall-clock runs of scenarios of one build
#   lab.sh cpu A B SCENARIO:FN...   callgrind marginal instructions per call of FN
#   lab.sh fleet NAME PODS direct|bouncer SCENARIO...
#                                   PODS processes at once (one pool each), totals
#   lab.sh pgss NAME SCENARIO...    server statement totals over the measured window (pg_stat_statements)
#   lab.sh cgwin NAME SCENARIO...   callgrind of the measured window only, split by function (cgsplit.py)
#   lab.sh perf NAME SCENARIO...    server CPU by PostgreSQL stage under load (perf, perfsplit.py)
#   lab.sh teardown                 stop the server and drop the lab network
#
# Environment: LAB_HOST (required, user@host of the lab), REPS [5], DURATION [3],
#   THREADS ["1 16"], POOL [8], DELAY_US [0] (added to every round trip),
#   BENCH_CPUS [1] (cpuset of the bench container; "0-1" for in-process contention)
#
# The server is pinned to CPU 0 and the bench to CPU 1, both at low CPU share,
# because the host also runs production services. Results land in
# ~/libpg-lab/results on the host and are printed as a markdown table.
set -eu
H=${LAB_HOST:?set LAB_HOST to the lab host as user@host}
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../.." && pwd)
BENCH_CPUS=${BENCH_CPUS:-1} REPS=${REPS:-5} DURATION=${DURATION:-3} THREADS=${THREADS:-"1 16"} POOL=${POOL:-8} DELAY_US=${DELAY_US:-0}

remote() { ssh -o BatchMode=yes "$H" "$@"; }
# Runs inner.sh in a fresh bench container.
# The server's cgroup is mounted read-only so the bench can report server CPU per
# operation (LIBPG_BENCH_SERVER_CPU, bench/harness.cpp).
inner() {
  remote "docker run --rm --network libpg-lab --cpuset-cpus $BENCH_CPUS --cpu-shares 256 --cap-add NET_ADMIN \
    -v \$HOME/libpg-lab:/lab -v /sys/fs/cgroup:/hostcg:ro \
    -e LIBPG_BENCH_SERVER_CPU=/hostcg/system.slice/docker-\$(docker inspect -f '{{.Id}}' libpg-lab-pg).scope/cpu.stat \
    ${INNER_ENV:-} libpg-lab nice -n 10 bash /lab/bin/inner.sh $*"
}

sync_tree() {
  rsync -az --delete --exclude 'build*' --exclude .git --exclude bench/results "$root/" "$H:libpg-lab/src/"
  rsync -az "$here/inner.sh" "$here/summarize.py" "$here/summarize_runs.py" "$H:libpg-lab/bin/"
}

case ${1:-} in
setup)
  remote 'mkdir -p ~/libpg-lab/bin ~/libpg-lab/v ~/libpg-lab/results ~/libpg-lab/image'
  rsync -az "$here/Dockerfile" "$H:libpg-lab/image/"
  sync_tree
  remote 'docker build -q -t libpg-lab ~/libpg-lab/image >/dev/null
    docker network inspect libpg-lab >/dev/null 2>&1 || docker network create libpg-lab >/dev/null
    if ! docker inspect libpg-lab-pg >/dev/null 2>&1; then
      docker run -d --name libpg-lab-pg --network libpg-lab --cpuset-cpus 0 --cpu-shares 256 --memory 512m \
        -e POSTGRES_HOST_AUTH_METHOD=trust -v libpg-lab-data:/var/lib/postgresql postgres:18 \
        -c shared_buffers=128MB -c max_connections=100 \
        -c shared_preload_libraries=pg_stat_statements >/dev/null
      until docker exec libpg-lab-pg pg_isready -q -U postgres; do sleep 1; done
      # Off by default (lab.sh pgss turns it on); set here, not with -c, so that
      # ALTER SYSTEM can change it.
      docker exec libpg-lab-pg psql -U postgres -qc "ALTER SYSTEM SET pg_stat_statements.track = '"'"'none'"'"'" \
        -c "SELECT pg_reload_conf()" >/dev/null
    fi
    docker exec libpg-lab-pg psql -U postgres -qtAc "SELECT 1 FROM pg_database WHERE datname = '"'"'bench'"'"'" | grep -q 1 ||
      { docker exec libpg-lab-pg createdb -U postgres bench &&
        docker exec -i libpg-lab-pg psql -U postgres -q -v ON_ERROR_STOP=1 -d bench < ~/libpg-lab/src/bench/schema.sql; }'
  echo "lab ready on $H" ;;
build)
  name=$2; shift 2
  sync_tree
  remote "rm -rf ~/libpg-lab/v/$name/src && mkdir -p ~/libpg-lab/v/$name && cp -a ~/libpg-lab/src ~/libpg-lab/v/$name/src"
  for p in "$@"; do
    rsync -az "$p" "$H:libpg-lab/v/$name/$(basename "$p")"
    remote "cd ~/libpg-lab/v/$name/src && patch -p1 -s < ../$(basename "$p")"
  done
  inner build "$name"
  echo "built $name" ;;
ab)
  a=$2 b=$3; shift 3
  out=results/$(date +%Y%m%d-%H%M%S)-$a-$b-d$DELAY_US.csv
  echo "variant,rep,scenario,threads,pool,ops,p50,p99,p999,extra" | remote "cat > ~/libpg-lab/$out"
  for rep in $(seq "$REPS"); do
    # Alternate which side goes first, so drift on the host hits both sides alike.
    if [ $((rep % 2)) = 1 ]; then pair="$a $b"; else pair="$b $a"; fi
    for v in $pair; do
      inner run "$v" "$DELAY_US" --duration "$DURATION" --threads "\"$THREADS\"" --pool "$POOL" "$*" |
        sed "s/^/$v,$rep,/" | remote "cat >> ~/libpg-lab/$out"
    done
    echo "rep $rep/$REPS done" >&2
  done
  echo "delay ${DELAY_US} µs/round trip, ${REPS} reps × ${DURATION}s, raw: ~/libpg-lab/$out"
  remote "python3 ~/libpg-lab/bin/summarize.py ~/libpg-lab/$out $a $b" ;;
load)
  remote "docker exec -i libpg-lab-pg psql -U postgres -q -v ON_ERROR_STOP=1 -d bench" < "$2"
  echo "loaded $2" ;;
run)
  name=$2; shift 2
  out=results/$(date +%Y%m%d-%H%M%S)-$name-d$DELAY_US.csv
  echo "variant,rep,scenario,threads,pool,ops,p50,p99,p999,extra" | remote "cat > ~/libpg-lab/$out"
  for rep in $(seq "$REPS"); do
    # One process per scenario, in an order rotated each rep, so drift on the host
    # hits every scenario alike.
    for s in "$@"; do
      inner run "$name" "$DELAY_US" --duration "$DURATION" --threads "\"$THREADS\"" --pool "$POOL" "$s" |
        sed "s/^/$name,$rep,/" | remote "cat >> ~/libpg-lab/$out"
    done
    set -- "${@:2}" "$1"
    echo "rep $rep/$REPS done" >&2
  done
  echo "delay ${DELAY_US} µs/round trip, ${REPS} reps × ${DURATION}s, raw: ~/libpg-lab/$out"
  remote "python3 ~/libpg-lab/bin/summarize_runs.py ~/libpg-lab/$out" ;;
cpu)
  a=$2 b=$3; shift 3
  for v in "$a" "$b"; do inner cpu "$v" "$*"; done ;;
fleet)
  name=$2 pods=$3 via=$4; shift 4
  echo "$name: $pods pods via $via, threads $THREADS, pool $POOL, delay ${DELAY_US} µs"
  inner fleet "$name" "$DELAY_US" "$pods" "$via" --duration "$DURATION" --threads "\"$THREADS\"" --pool "$POOL" "$*" |
    awk -F, -v pods="$pods" '
      NF >= 8 { k = $2 " threads=" $3; n[k]++; ops[k] += $5; if ($7 > p99[k]) p99[k] = $7; if ($8 > p999[k]) p999[k] = $8 }
      END { for (k in n) printf "%s: %d of %d pods ran, total %.0f ops/s, worst p99 %.0f µs, worst p999 %.0f µs\n", k, n[k], pods, ops[k], p99[k], p999[k] }' ;;
pgss)
  # Tracking is on only for these runs: it costs the server per statement.
  name=$2; shift 2
  remote "docker exec libpg-lab-pg psql -U postgres -d bench -qc 'CREATE EXTENSION IF NOT EXISTS pg_stat_statements' \
    -c \"ALTER SYSTEM SET pg_stat_statements.track = 'top'\" -c 'SELECT pg_reload_conf()' >/dev/null"
  for s in "$@"; do
    INNER_ENV="-e LIBPG_BENCH_PGSS=postgresql://postgres@libpg-lab-pg/bench" \
      inner run "$name" "$DELAY_US" --duration "$DURATION" --threads "\"$THREADS\"" --pool "$POOL" "$s" 2>&1 |
      grep -E '^(pgss|real_|v[0-9])' | grep -v ' pool=' || true
  done
  remote "docker exec libpg-lab-pg psql -U postgres -d bench -qc \"ALTER SYSTEM SET pg_stat_statements.track = 'none'\" \
    -c 'SELECT pg_reload_conf()' >/dev/null" ;;
perf)
  # Server CPU by PostgreSQL stage (perfsplit.py): the bench saturates the server
  # with PERF_THREADS [16] while perf samples only the server's cgroup, inside the
  # bench's measured window; the bench's server CPU per operation scales it.
  name=$2; shift 2
  rsync -az "$here/perfsplit.py" "$H:libpg-lab/bin/"
  for s in "$@"; do
    echo "## $s"
    remote "id=\$(docker inspect -f '{{.Id}}' libpg-lab-pg)
      docker run -d --name libpg-lab-perfload --network libpg-lab --cpuset-cpus $BENCH_CPUS --cpu-shares 256 \
        -v \$HOME/libpg-lab:/lab -v /sys/fs/cgroup:/hostcg:ro \
        -e LIBPG_BENCH_SERVER_CPU=/hostcg/system.slice/docker-\$id.scope/cpu.stat libpg-lab \
        bash -c '/lab/v/$name/build/bench/pg_bench --dsn postgresql://postgres@libpg-lab-pg/bench --pool $POOL \
          --threads ${PERF_THREADS:-16} --duration 25 $s 2>/dev/null | grep ^$s, > /lab/results/perf-$s.line' >/dev/null
      sleep 3
      sudo -n perf record -q -o /tmp/pgperf.data -a -e cpu-clock -G system.slice/docker-\$id.scope -F 999 \
        --call-graph dwarf,32768 -- sleep 3 >/dev/null 2>&1
      # Reported while the load runs: perf finds the server's binary only through
      # live backends (it is not at that path on the host).
      sudo -n perf report -i /tmp/pgperf.data --stdio --children --sort sym -g none --percent-limit 0.2 2>/dev/null > /tmp/pgperf.txt
      docker wait libpg-lab-perfload >/dev/null; docker rm libpg-lab-perfload >/dev/null
      cat ~/libpg-lab/results/perf-$s.line
      python3 ~/libpg-lab/bin/perfsplit.py /tmp/pgperf.txt \$(grep -oE 'srv_us=[0-9.]+' ~/libpg-lab/results/perf-$s.line | cut -d= -f2)"
  done ;;
cgwin)
  name=$2; shift 2
  out=results/cgwin-$name-$(date +%Y%m%d-%H%M%S)
  remote "mkdir -p ~/libpg-lab/$out"
  rsync -az "$here/cgsplit.py" "$H:libpg-lab/bin/"
  for s in "$@"; do inner cgwin "$name" "/lab/$out" "$DURATION" "$s"; done
  remote "cd ~/libpg-lab/$out && for f in *.window; do python3 ~/libpg-lab/bin/cgsplit.py \$f; done"
  echo "dumps: ~/libpg-lab/$out" ;;
teardown)
  remote 'docker rm -f libpg-lab-pg >/dev/null 2>&1; docker network rm libpg-lab >/dev/null 2>&1; true'
  echo "lab stopped (volume libpg-lab-data kept)" ;;
*) sed -n '2,20p' "$0"; exit 2 ;;
esac
