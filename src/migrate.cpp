#include <map>
#include <memory>

#include "pg/migrate.hpp"

namespace pg {
namespace {

using result = std::unique_ptr<PGresult, detail::result_deleter>;

// Runs a statement with text parameters, or a multi-statement script without
// them, on a blocking connection; a failure throws pg::error named after `what`.
result run(PGconn* c, const std::string& what, std::string_view text, std::vector<const char*> params = {}) {
    const std::string s(text);
    result r(params.empty() ? PQexec(c, s.c_str())
                            : PQexecParams(c, s.c_str(), static_cast<int>(params.size()), nullptr, params.data(),
                                           nullptr, nullptr, 0));
    const ExecStatusType st = PQresultStatus(r.get());
    if (st == PGRES_COMMAND_OK || st == PGRES_TUPLES_OK || st == PGRES_EMPTY_QUERY) return r;
    const char* state = PQresultErrorField(r.get(), PG_DIAG_SQLSTATE);
    const char* primary = PQresultErrorField(r.get(), PG_DIAG_MESSAGE_PRIMARY);
    const std::string code = state ? state : "", msg = primary ? primary : PQerrorMessage(c);
    throw error(code.empty() && PQstatus(c) != CONNECTION_OK ? error_kind::broken : classify(code, msg),
                what + " failed: " + msg, code);
}

// The text with CRLF turned into LF and trailing whitespace stripped from every line.
std::string normalized(std::string_view text) {
    std::string out;
    for (std::size_t pos = 0; pos <= text.size();) {
        const std::size_t nl = std::min(text.find('\n', pos), text.size());
        std::string_view line = text.substr(pos, nl - pos);
        while (!line.empty() && std::string_view(" \t\r\f\v").find(line.back()) != std::string_view::npos)
            line.remove_suffix(1);
        out += line;
        if (nl == text.size()) break;
        out += '\n';
        pos = nl + 1;
    }
    return out;
}

void apply(PGconn* c, const migrate_options& o) {
    run(c, "migrate lock", "SELECT pg_advisory_xact_lock(" + std::to_string(0x6c69627067LL) + ")");
    run(c, "migrate setup", "CREATE SCHEMA IF NOT EXISTS libpg");
    run(c, "migrate setup",
        "CREATE TABLE IF NOT EXISTS libpg.migrations (name text PRIMARY KEY, sha256 text NOT NULL, "
        "applied_at timestamptz NOT NULL DEFAULT now())");
    std::map<std::string, std::string, std::less<>> applied;
    const result rows = run(c, "migrate setup", "SELECT name, sha256 FROM libpg.migrations");
    for (int i = 0; i < PQntuples(rows.get()); ++i) applied.emplace(PQgetvalue(rows.get(), i, 0), PQgetvalue(rows.get(), i, 1));
    for (const migration& m : o.migrations) {
        const std::string name(m.name), text = normalized(m.sql);
        const result sum = run(c, "migration " + name, "SELECT encode(sha256(convert_to($1, 'UTF8')), 'hex')", {text.c_str()});
        const std::string checksum = PQgetvalue(sum.get(), 0, 0);
        if (const auto it = applied.find(name); it != applied.end()) {
            if (it->second != checksum) throw error(error_kind::other, "migration " + name + " changed after it was applied");
            continue;
        }
        run(c, "migration " + name, m.sql);
        if (PQtransactionStatus(c) != PQTRANS_INTRANS)
            throw error(error_kind::other, "migration " + name + " must not end the transaction");
        run(c, "migration " + name, "INSERT INTO libpg.migrations (name, sha256) VALUES ($1, $2)",
            {name.c_str(), checksum.c_str()});
    }
    run(c, "migrate commit", "COMMIT");
}

}  // namespace

void migrate(pool& p, const migrate_options& o) {
    conn cn(p);
    PGconn* c = cn.raw();
    run(c, "migrate begin", "BEGIN");
    try {
        apply(c, o);
    } catch (...) {
        PQclear(PQexec(c, "ROLLBACK"));
        throw;
    }
}

}  // namespace pg
