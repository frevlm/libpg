# Build-and-bench image of the remote lab: g++, cmake, libpq 18, valgrind, tc (netem).
FROM ubuntu:24.04
RUN apt-get update && apt-get install -y --no-install-recommends ca-certificates curl gnupg g++ cmake make catch2 rsync valgrind  && install -d /usr/share/postgresql-common/pgdg  && curl -fsSL https://www.postgresql.org/media/keys/ACCC4CF8.asc -o /usr/share/postgresql-common/pgdg/apt.postgresql.org.asc  && echo "deb [signed-by=/usr/share/postgresql-common/pgdg/apt.postgresql.org.asc] https://apt.postgresql.org/pub/repos/apt noble-pgdg main" > /etc/apt/sources.list.d/pgdg.list  && apt-get update && apt-get install -y --no-install-recommends libpq-dev postgresql-client-18  && rm -rf /var/lib/apt/lists/*
RUN apt-get update && apt-get install -y --no-install-recommends iproute2 && rm -rf /var/lib/apt/lists/*
WORKDIR /lab
