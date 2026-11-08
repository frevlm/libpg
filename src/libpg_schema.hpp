// The objects libpg keeps in the database, in the `libpg` schema (ADR 0017).
#pragma once
#include <cstdint>
#include <string>
#include <string_view>

namespace pg::detail {

// The shared prefix of a watched column's four trigger names:
// libpg_w_<first 16 hex of sha256(table || '.' || column)>.
std::string watch_trigger_base(std::string_view table, std::string_view column);

// Advisory lock key that serializes migrations: "libpg" in ASCII (0x6c69627067).
// Taken with pg_advisory_xact_lock by pg::migrate.
inline constexpr std::int64_t migrate_lock_key = 0x6c69627067;

// NOTIFY channel of the watched-column triggers.
inline constexpr std::string_view watch_channel = "libpg_watch";

// The idempotent DDL of the `libpg` schema, as a multi-statement script:
//   libpg.migrations(name text PK, sha256 text, applied_at timestamptz)
//   libpg.flush_receipts(id uuid PK, created_at timestamptz) + index on created_at
//   libpg.watch_notify(), the trigger function of watched columns.
std::string_view libpg_schema_sql();

// Watched-column notification contract (the write-back cache depends on it).
//
// A watched column c of table t whose key columns are k1..kn has four
// statement-level AFTER triggers, all running libpg.watch_notify('c','k1',..,'kn'),
// named libpg_w_<first 16 hex of sha256(t || '.' || c)>_<suffix>:
//   _ins  AFTER INSERT    REFERENCING NEW TABLE
//   _upd  AFTER UPDATE    REFERENCING OLD TABLE NEW TABLE
//   _del  AFTER DELETE    REFERENCING OLD TABLE
//   _trn  AFTER TRUNCATE
// (PostgreSQL allows transition tables neither with a column list nor with
// several events, hence separate triggers.)
//
// Each firing sends pg_notify('libpg_watch', payload) with the compact JSON
//   {"t":"<table>","c":"<column>","k":[["<k1>","<k2>"],...]}
// where every key value is its text form. The keys are those of the inserted
// rows, the deleted rows, or the updated rows whose column IS DISTINCT FROM the
// old value; new and old rows are joined (FULL JOIN) on the key columns, so an
// update that changes a key reports both the old and the new key. One firing
// splits into several notifications so that each payload is at most 7000 bytes
// (a single oversized key aside). Nothing is sent when no row qualifies.
// TRUNCATE sends {"t":"<table>","c":"<column>","all":true}: every key changed.
}  // namespace pg::detail
