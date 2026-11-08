// List-page reads: wide multi-row result sets, raw libpq against pg::conn::query into a read shape.
#include <array>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "harness.hpp"
#include "pg/pg.hpp"
#include "raw.hpp"

namespace bench {
namespace {

constexpr std::int64_t kUsers = 1000;
constexpr int kListRows = 50;
constexpr int kBinary = 1;

constexpr const char* kListSql =
    "SELECT id, user_id, problem_id, submitter, title, note, score, submit_time, accepted, content "
    "FROM submissions WHERE user_id = $1 ORDER BY id DESC LIMIT 50";
constexpr const char* kGetSql =
    "SELECT id, user_id, problem_id, submitter, title, note, score, submit_time, accepted, content "
    "FROM submissions WHERE id = $1";

using money = pg::decimal<6>;

// The read shape in the form pg_tablegen writes it.
struct submission_row {
    std::int64_t id{};
    std::int64_t user_id{};
    std::int32_t problem_id{};
    std::string submitter{};
    std::string title{};
    std::optional<std::string> note{};
    money score{};
    pg::timestamptz submit_time{};
    bool accepted{};
    pg::jsonb content{};
    static constexpr std::size_t pg_columns = 10;
    template <class F>
    void pg_each(F&& f) {
        f(id);
        f(user_id);
        f(problem_id);
        f(submitter);
        f(title);
        f(note);
        f(score);
        f(submit_time);
        f(accepted);
        f(content);
    }
};
static_assert(pg::read_shape<submission_row>);

// --- raw ----------------------------------------------------------------------------

// Big-endian int4.
std::int64_t be32(const char* p) {
    std::uint32_t v = 0;
    for (int i = 0; i < 4; ++i) v = (v << 8) | static_cast<std::uint8_t>(p[i]);
    return static_cast<std::int32_t>(v);
}

// Copies one text cell.
std::string cell_text(const PGresult* r, int row, int col, int skip = 0) {
    return std::string(PQgetvalue(r, row, col) + skip, static_cast<std::size_t>(PQgetlength(r, row, col) - skip));
}

// Decodes every cell of one row the way the read shape does.
std::uint64_t touch_row(const PGresult* r, int i) {
    std::uint64_t s = static_cast<std::uint64_t>(raw::be64(PQgetvalue(r, i, 0)) + raw::be64(PQgetvalue(r, i, 1)));
    s += static_cast<std::uint64_t>(be32(PQgetvalue(r, i, 2)));
    s += cell_text(r, i, 3).size() + cell_text(r, i, 4).size();
    if (!PQgetisnull(r, i, 5)) s += cell_text(r, i, 5).size();
    s += static_cast<std::uint64_t>(raw::numeric_micros(PQgetvalue(r, i, 6)));
    s += static_cast<std::uint64_t>(raw::be64(PQgetvalue(r, i, 7)));
    s += static_cast<std::uint64_t>(*PQgetvalue(r, i, 8));
    s += cell_text(r, i, 9, 1).size();  // jsonb binary starts with a version byte
    return s;
}

std::uint64_t touch_rows(const PGresult* r, int want) {
    if (PQntuples(r) != want) throw std::runtime_error("list: wrong row count");
    std::uint64_t s = 0;
    for (int i = 0; i < want; ++i) s += touch_row(r, i);
    return s;
}

void run_raw(const Cfg& cfg, const char* name, const char* sql, int rows, std::int64_t keys) {
    for (int t : cfg.threads) {
        note(std::string(name) + " threads=" + std::to_string(t));
        emit(name, t, 0, run_point(t, cfg.duration, [&](Ctx& x) {
                 raw::Link link(cfg.dsn);
                 const Oid types[1] = {raw::kInt8Oid};
                 raw::prepare(link.c, "q", sql, 1, types);
                 x.loop([&] {
                     const raw::BinInt8 key(x.rng.key(keys));
                     const char* v[1] = {key.buf};
                     const int len[1] = {8}, fmt[1] = {kBinary};
                     const raw::PgResult r{PQexecPrepared(link.c, "q", 1, v, len, fmt, kBinary)};
                     raw::expect(r, PGRES_TUPLES_OK, link.c);
                     x.sink += touch_rows(r.get(), rows);
                     return static_cast<std::uint64_t>(1);
                 });
             }));
    }
}

void run_raw_list_50(const Cfg& cfg) { run_raw(cfg, "raw_list_50", kListSql, kListRows, kUsers); }
void run_raw_list_1(const Cfg& cfg) { run_raw(cfg, "raw_list_1", kGetSql, 1, kRows); }

// --- v2 -----------------------------------------------------------------------------

std::uint64_t touch_shape(const submission_row& r) {
    return static_cast<std::uint64_t>(r.id + r.user_id + r.problem_id + r.score.units + r.submit_time.time_since_epoch().count() +
                                      r.accepted) +
           r.submitter.size() + r.title.size() + (r.note ? r.note->size() : 0) + r.content.text.size();
}

void run_v2(const Cfg& cfg, const char* name, bool list) {
    for (int size : cfg.pools)
        for (int t : cfg.threads) {
            note(std::string(name) + " pool=" + std::to_string(size) + " threads=" + std::to_string(t));
            pg::pool_options o;
            o.dsn = cfg.dsn;
            o.pooler = cfg.pooler;
            o.max_conns = size;
            pg::pool p(o);
            emit(name, t, size, run_point(t, cfg.duration, [&](Ctx& x) {
                     x.loop([&] {
                         pg::conn c(p);
                         if (!list) {
                             const auto rows = c.query<submission_row>(
                                 pg::sql{"SELECT id, user_id, problem_id, submitter, title, note, score, submit_time, "
                                         "accepted, content FROM submissions WHERE id = {}",
                                         x.rng.key()});
                             if (rows.size() != 1) throw std::runtime_error("v2_get_row: wrong row count");
                             x.sink += touch_shape(rows[0]);
                             return;
                         }
                         const auto rows = c.query<submission_row>(
                             pg::sql{"SELECT id, user_id, problem_id, submitter, title, note, score, submit_time, "
                                     "accepted, content FROM submissions WHERE user_id = {} ORDER BY id DESC LIMIT 50",
                                     x.rng.key(kUsers)});
                         if (rows.size() != kListRows) throw std::runtime_error("v2_list_50: wrong row count");
                         for (const auto& r : rows) x.sink += touch_shape(r);
                     });
                 }));
        }
}

void run_v2_list_50(const Cfg& cfg) { run_v2(cfg, "v2_list_50", true); }

// v2_get_row next to one slow statement: worker 0 loops a 50 ms pg_sleep (not
// measured), the others read rows. Shows whether short statements queue behind
// the slow one.
void run_v2_get_row_slow(const Cfg& cfg) {
    for (int size : cfg.pools)
        for (int t : cfg.threads) {
            if (t < 2) continue;
            note("v2_get_row_slow pool=" + std::to_string(size) + " threads=" + std::to_string(t));
            pg::pool_options o;
            o.dsn = cfg.dsn;
            o.pooler = cfg.pooler;
            o.max_conns = size;
            pg::pool p(o);
            emit("v2_get_row_slow", t - 1, size, run_point(t, cfg.duration, [&](Ctx& x) {
                     if (x.tid == 0) {
                         x.ready();
                         while (x.running()) {
                             pg::conn c(p);
                             c.exec(pg::sql{"SELECT pg_sleep(0.05)"});
                         }
                         return;
                     }
                     x.loop([&] {
                         pg::conn c(p);
                         const auto rows = c.query<submission_row>(
                             pg::sql{"SELECT id, user_id, problem_id, submitter, title, note, score, submit_time, "
                                     "accepted, content FROM submissions WHERE id = {}",
                                     x.rng.key()});
                         x.sink += touch_shape(rows.at(0));
                     });
                 }));
        }
}
void run_v2_get_row(const Cfg& cfg) { run_v2(cfg, "v2_get_row", false); }

}  // namespace

void register_list(std::vector<Scenario>& v) {
    v.push_back({"raw_list_50", run_raw_list_50});
    v.push_back({"v2_list_50", run_v2_list_50});
    v.push_back({"raw_list_1", run_raw_list_1});
    v.push_back({"v2_get_row", run_v2_get_row});
    v.push_back({"v2_get_row_slow", run_v2_get_row_slow});
}

}  // namespace bench
