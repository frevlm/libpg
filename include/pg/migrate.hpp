#pragma once
#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "pool.hpp"

namespace pg {

// A migration script: a multi-statement SQL text and the name it is recorded under.
struct migration {
    std::string_view name;
    std::string_view sql;
};

// A column whose changes are announced on the `libpg_watch` NOTIFY channel (ADR 0013).
struct watched_column {
    std::string table, column;
    std::vector<std::string> key_columns;
};

// The watched column of a generated column tag.
template <class Col>
watched_column watch() {
    watched_column w{std::string(Col::pg_table::name), std::string(Col::pg_name), {}};
    for (std::string_view k : Col::pg_table::key_columns) w.key_columns.emplace_back(k);
    return w;
}

// The bookkeeping table a project kept before libpg: (version int, name text).
struct legacy_import {
    std::string table = "schema_migrations";
};

struct migrate_options {
    std::vector<migration> migrations;     // applied in this order
    std::vector<watched_column> watched;  // triggers kept in sync on every run
    std::optional<legacy_import> legacy;  // imported on the first run
    std::optional<std::int64_t> legacy_lock;  // the old binary's advisory lock, taken too
    std::chrono::milliseconds lock_timeout{2'000};
    std::chrono::milliseconds statement_timeout{0};  // SET LOCAL statement_timeout; 0 = none
    int attempts = 10;  // runs on lock timeout (55P03), with backoff in between
};

// Brings the database up to date in ONE transaction, under advisory locks:
//   1. SET LOCAL lock_timeout; take libpg's advisory lock (key 0x6c69627067, "libpg")
//      and `legacy_lock` when given;
//   2. create the `libpg` schema (migrations, flush receipts, trigger function);
//   3. on the first run, import the applied migrations of the legacy table (read only);
//   4. verify the checksums of the applied migrations (a changed one throws error_kind::other);
//   5. run the pending migrations in order and record their checksums;
//   6. sync the watched-column triggers; COMMIT.
// A lock timeout rolls everything back and reruns after a jittered backoff, up to `attempts`.
//
// Why the work runs on a pooled connection: scripts are multi-statement, which only the
// simple protocol runs. The transaction is opened through the engine (pipelined), then the
// script runner leaves pipeline mode, runs the script inside that same transaction and
// re-enters pipeline mode. That connection is marked discard (ADR 0004), so the pool closes
// it afterwards and no connection that ran a script is ever reused.
void migrate(pool& p, const migrate_options& o);

}  // namespace pg
