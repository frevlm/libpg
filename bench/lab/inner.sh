#!/bin/bash
# Runs inside the lab container; /lab is the host's ~/libpg-lab.
#   inner.sh build NAME
#   inner.sh run NAME DELAY_US pg_bench-args...
#   inner.sh cpu NAME scenario:normalizer...
#   inner.sh cgwin NAME OUTDIR DURATION scenario
#   inner.sh fleet NAME DELAY_US PODS direct|bouncer pg_bench-args...
set -eu
DSN=postgresql://postgres@libpg-lab-pg/bench
cmd=$1 name=$2; shift 2
bin=/lab/v/$name/build/bench/pg_bench
case $cmd in
build)
  cmake -S /lab/v/$name/src -B /lab/v/$name/build -DCMAKE_BUILD_TYPE=Release -DLIBPG_BENCH=ON >/dev/null
  cmake --build /lab/v/$name/build -j2 --target pg_bench >/dev/null ;;
run)
  delay=$1; shift
  # netem delays this container's egress: every round trip to the server gains `delay`.
  [ "$delay" = 0 ] || tc qdisc add dev eth0 root netem delay "${delay}us"
  exec "$bin" --dsn "$DSN" "$@" ;;
fleet)
  # PODS processes at once, each a pod with its own pool, straight to the
  # server or through PgBouncer (transaction mode, 16 server connections).
  delay=$1 pods=$2 via=$3; shift 3
  [ "$delay" = 0 ] || tc qdisc add dev eth0 root netem delay "${delay}us"
  dsn=$DSN
  [ "$via" = bouncer ] && dsn=postgresql://postgres@libpg-lab-bouncer:6432/bench
  for i in $(seq "$pods"); do
    "$bin" --dsn "$dsn" "$@" 2>/dev/null | sed "s/^/$i,/" &
  done
  wait ;;
cgwin)
  # Callgrind of the measured window only: the harness zeroes the counters when
  # the window opens and dumps them when it closes (part 1); the run line
  # carries the window's operation count.
  out=$1 duration=$2 s=$3
  line=$(valgrind --tool=callgrind --callgrind-out-file="$out/$s.%p" "$bin" --dsn "$DSN" --pool 8 --threads 1 \
    --duration "$duration" "$s" 2>/dev/null | grep "^$s,")
  ops=$(echo "$line" | grep -oE 'ops=[0-9]+' | cut -d= -f2)
  for f in "$out/$s".*.1; do mv "$f" "$out/$s.ops$ops.window" && chmod a+r "$out/$s.ops$ops.window"; done
  rm -f "$out/$s".[0-9]*
  echo "$line" ;;
cpu)
  # Marginal instructions per call of a normalizer: d(total)/d(calls) between a 1 s
  # and a 3 s run, which cancels connection setup and warmup.
  tot() { callgrind_annotate "$1" 2>/dev/null | awk '/PROGRAM TOTALS/ {gsub(",","",$1); print $1}'; }
  calls() { callgrind_annotate --tree=caller "$1" 2>/dev/null | awk -v fn="$2" '
    /^$/ {blk=""; next}
    / < / {blk=blk"\n"$0; next}
    / \* / && index($0, fn) { s=0; m=split(blk, L, "\n"); for(i=1;i<=m;i++) if (match(L[i], /\(([0-9,]+)x\)/)) { v=substr(L[i], RSTART+1, RLENGTH-3); gsub(",","",v); s+=v }; print s; exit }'; }
  for pair in "$@"; do
    s=${pair%%:*} f=${pair#*:}
    for d in 1 3; do
      valgrind --tool=callgrind --callgrind-out-file=/tmp/cg.$d "$bin" --dsn "$DSN" --pool 8 --threads 1 --duration $d "$s" >/dev/null 2>&1
    done
    t1=$(tot /tmp/cg.1) t3=$(tot /tmp/cg.3) n1=$(calls /tmp/cg.1 "$f") n3=$(calls /tmp/cg.3 "$f")
    if [ -z "$n1" ] || [ -z "$n3" ] || [ "$n3" = "$n1" ]; then echo "$s,$name,NA"; continue; fi
    echo "$s,$name,$(( (t3 - t1) / (n3 - n1) ))"
  done ;;
esac
