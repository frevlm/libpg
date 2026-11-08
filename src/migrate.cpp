#include "pg/migrate.hpp"

#include <algorithm>
#include <map>
#include <random>
#include <set>
#include <thread>
#include <tuple>

#include "engine.hpp"
#include "libpg_schema.hpp"
#include "pg/error.hpp"
#include "pg/pool.hpp"
#include "pg/sql.hpp"
#include "sha256.hpp"

namespace pg {
namespace {

using detail::session_of;

// A statement's message without the "SQLSTATE: " prefix of error::what().
std::string message_of(const error& e) {
    std::string m = e.what();
    const std::string prefix = e.sqlstate + ": ";
    if (!e.sqlstate.empty() && m.starts_with(prefix)) m.erase(0, prefix.size());
    return m;
}

void check_options(const migrate_options& o) {
    if (o.attempts < 1) throw error(error_kind::other, "migrate: attempts must be at least 1");
    std::set<std::string_view> names;
    for (const migration& m : o.migrations)
        if (!names.insert(m.name).second)
            throw error(error_kind::other, "migrate: duplicate migration name " + std::string(m.name));
}

void take_locks(conn& c, const migrate_options& o) {
    c.exec(unsafe_sql("SET LOCAL lock_timeout = " + std::to_string(o.lock_timeout.count())));
    c.exec(unsafe_sql("SET LOCAL statement_timeout = " + std::to_string(o.statement_timeout.count())));
    c.exec(sql{"SELECT pg_advisory_xact_lock({})", detail::migrate_lock_key});
    if (o.legacy_lock) c.exec(sql{"SELECT pg_advisory_xact_lock({})", *o.legacy_lock});
}

using checksums = std::map<std::string, std::string, std::less<>>;

checksums applied_migrations(conn& c) {
    checksums out;
    for (auto& [name, sum] : c.query<std::string, std::string>(sql{"SELECT name, sha256 FROM libpg.migrations"}))
        out.emplace(std::move(name), std::move(sum));
    return out;
}

void record(conn& c, const migration& m) {
    c.exec(sql{"INSERT INTO libpg.migrations (name, sha256) VALUES ({}, {})", m.name,
               detail::migration_checksum(m.sql)});
}

// The migration name without ".sql" and without a leading "<digits>_".
std::string_view bare_name(std::string_view n) {
    if (n.ends_with(".sql")) n.remove_suffix(4);
    std::size_t i = 0;
    while (i < n.size() && n[i] >= '0' && n[i] <= '9') ++i;
    if (i > 0 && i < n.size() && n[i] == '_') n.remove_prefix(i + 1);
    return n;
}

// The integer a name starts with, -1 if it starts with no digit.
std::int64_t leading_int(std::string_view n) {
    std::int64_t v = 0;
    std::size_t i = 0;
    for (; i < n.size() && n[i] >= '0' && n[i] <= '9' && i < 15; ++i) v = v * 10 + (n[i] - '0');
    return i == 0 ? -1 : v;
}

// Index of the migration a legacy row stands for: same bare name, else same version.
std::optional<std::size_t> match_legacy(const std::vector<migration>& ms, std::int64_t version, std::string_view name) {
    for (std::size_t i = 0; i < ms.size(); ++i)
        if (bare_name(ms[i].name) == bare_name(name)) return i;
    for (std::size_t i = 0; i < ms.size(); ++i)
        if (leading_int(ms[i].name) == version) return i;
    return std::nullopt;
}

// Records the migrations the legacy table says were applied. The legacy table is only read.
void import_legacy(conn& c, const migrate_options& o) {
    const auto reg = c.query_one<std::optional<std::string>>(sql{"SELECT to_regclass({})::text", o.legacy->table});
    if (!reg) return;
    const auto rows = c.query<std::int64_t, std::string>(unsafe_sql("SELECT version::int8, name FROM " + *reg));
    std::set<std::size_t> done;
    for (const auto& [version, name] : rows)
        if (auto i = match_legacy(o.migrations, version, name)) done.insert(*i);
    for (std::size_t i : done) record(c, o.migrations[i]);
}

void verify(const migrate_options& o, const checksums& applied) {
    for (const migration& m : o.migrations) {
        const auto it = applied.find(m.name);
        if (it != applied.end() && it->second != detail::migration_checksum(m.sql))
            throw error(error_kind::other, "migration " + std::string(m.name) + " changed after it was applied");
    }
}

void run_migration(conn& c, const migration& m) {
    try {
        detail::run_script(session_of(c), m.sql);
    } catch (const error& e) {
        throw error(e.kind, "migration " + std::string(m.name) + " failed: " + message_of(e), e.sqlstate, e.sql);
    }
    if (PQtransactionStatus(session_of(c).c) != PQTRANS_INTRANS)
        throw error(error_kind::other, "migration " + std::string(m.name) + " must not end the transaction");
    record(c, m);
}

std::string literal(std::string_view s) {
    std::string out = "'";
    for (char ch : s) {
        if (ch == '\'') out += '\'';
        out += ch;
    }
    return out + '\'';
}

std::string hex_of(std::string_view bytes) {
    static constexpr char digits[] = "0123456789abcdef";
    std::string out;
    for (unsigned char b : bytes) {
        out += digits[b >> 4];
        out += digits[b & 0xf];
    }
    return out;
}

// One trigger to exist: its name, table (as the server prints it), event, transition tables and arguments.
struct trigger_spec {
    std::string name, table, event, referencing, args_sql, args_hex;
};

struct trigger_id {
    std::string name, table;
    bool operator<(const trigger_id& o) const { return std::tie(name, table) < std::tie(o.name, o.table); }
};

std::vector<trigger_spec> triggers_of(conn& c, const watched_column& w) {
    const auto table = c.query_one<std::optional<std::string>>(sql{"SELECT to_regclass({})::text", detail::quote_ident(w.table)});
    if (!table) throw error(error_kind::other, "watched table " + w.table + " does not exist");
    std::string args_sql = literal(w.column);
    std::string args_raw = w.column + '\0';
    for (const std::string& k : w.key_columns) {
        args_sql += ", " + literal(k);
        args_raw += k + '\0';
    }
    const std::string base = detail::watch_trigger_base(w.table, w.column);
    const std::string hex = hex_of(args_raw);
    return {{base + "_ins", *table, "INSERT", "REFERENCING NEW TABLE AS libpg_new", args_sql, hex},
            {base + "_upd", *table, "UPDATE", "REFERENCING OLD TABLE AS libpg_old NEW TABLE AS libpg_new", args_sql, hex},
            {base + "_del", *table, "DELETE", "REFERENCING OLD TABLE AS libpg_old", args_sql, hex},
            {base + "_trn", *table, "TRUNCATE", "", args_sql, hex}};
}

std::map<trigger_id, std::string> existing_triggers(conn& c) {
    std::map<trigger_id, std::string> out;
    const auto rows = c.query<std::string, std::string, std::string>(sql{
        "SELECT t.tgname::text, t.tgrelid::regclass::text, encode(t.tgargs, 'hex') FROM pg_trigger t "
        "WHERE t.tgfoid = 'libpg.watch_notify'::regproc"});
    for (const auto& [name, table, hex] : rows) out[{name, table}] = hex;
    return out;
}

void create_trigger(conn& c, const trigger_spec& t) {
    c.exec(unsafe_sql("CREATE OR REPLACE TRIGGER " + detail::quote_ident(t.name) + " AFTER " + t.event + " ON " + t.table + " " + t.referencing +
                      " FOR EACH STATEMENT EXECUTE FUNCTION libpg.watch_notify(" + t.args_sql + ")"));
}

// Makes the libpg triggers match `watched`: missing or changed ones are created, others dropped.
void sync_triggers(conn& c, const migrate_options& o) {
    auto existing = existing_triggers(c);
    for (const watched_column& w : o.watched) {
        for (const trigger_spec& t : triggers_of(c, w)) {
            const auto it = existing.find({t.name, t.table});
            if (it == existing.end() || it->second != t.args_hex) create_trigger(c, t);
            if (it != existing.end()) existing.erase(it);
        }
    }
    for (const auto& [id, hex] : existing)
        c.exec(unsafe_sql("DROP TRIGGER " + detail::quote_ident(id.name) + " ON " + id.table));
}

void migrate_once(pool& p, const migrate_options& o) {
    conn c(p);
    transaction tx(c);
    take_locks(c, o);
    detail::run_script(session_of(c), detail::libpg_schema_sql());
    auto applied = applied_migrations(c);
    if (applied.empty() && o.legacy) {
        import_legacy(c, o);
        applied = applied_migrations(c);
    }
    verify(o, applied);
    for (const migration& m : o.migrations)
        if (!applied.contains(m.name)) run_migration(c, m);
    sync_triggers(c, o);
    tx.commit();
}

// Sleeps 50 ms doubling per attempt (cap 1 s), times a random factor in [0.5, 1).
void backoff(int attempt) {
    std::minstd_rand rng(std::random_device{}());
    const int base = std::min(50 << std::min(attempt - 1, 5), 1000);
    std::this_thread::sleep_for(std::chrono::milliseconds(base / 2 + static_cast<int>(rng() % static_cast<unsigned>(base / 2))));
}

}  // namespace

void migrate(pool& p, const migrate_options& o) {
    check_options(o);
    for (int attempt = 1;; ++attempt) {
        try {
            return migrate_once(p, o);
        } catch (const error& e) {
            if (e.sqlstate != "55P03" || attempt >= o.attempts) throw;
        }
        backoff(attempt);
    }
}

}  // namespace pg
