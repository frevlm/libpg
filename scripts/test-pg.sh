#!/bin/sh
# A throwaway PostgreSQL for ctest: `start` makes a fresh cluster in DIR and
# serves it on 127.0.0.1:PORT with a database libpg_test, `stop` ends it and
# removes DIR. BINDIR holds initdb and pg_ctl.
set -eu

if [ $# -ne 4 ]; then
  echo "usage: $0 start|stop DIR PORT BINDIR" >&2
  exit 2
fi
action=$1 dir=$2 port=$3 bin=$4
# On macOS the postmaster refuses to start without a valid LC_ALL.
export LC_ALL=C

stop() {
  if [ -f "$dir/postmaster.pid" ]; then
    "$bin/pg_ctl" -D "$dir" -m immediate -w stop >/dev/null
  fi
  rm -rf "$dir"
}

case $action in
start)
  # A cluster a killed run left behind.
  stop
  "$bin/initdb" -D "$dir" -U postgres -A trust -E UTF8 --no-locale --no-sync >/dev/null
  "$bin/pg_ctl" -D "$dir" -l "$dir/server.log" -w \
    -o "-p $port -c listen_addresses=127.0.0.1 -c unix_socket_directories= -c fsync=off" start >/dev/null ||
    { cat "$dir/server.log" >&2; exit 1; }
  "$bin/createdb" -h 127.0.0.1 -p "$port" -U postgres libpg_test
  ;;
stop)
  stop
  ;;
*)
  echo "usage: $0 start|stop DIR PORT BINDIR" >&2
  exit 2
  ;;
esac
