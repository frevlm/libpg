// Read shapes of two applications (HHHOJ online judge, revlm API gateway), each as the
// application issues them today and in fewer-round-trip or less-data forms.
#include <array>
#include <atomic>
#include <chrono>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <tuple>
#include <vector>

#include "harness.hpp"
#include "pg/pg.hpp"

namespace bench {
namespace {

using money = pg::decimal<6>;

constexpr std::int64_t kOjUsers = 10'000;
constexpr std::int64_t kSubUsers = 1'000;
constexpr std::int64_t kRvUsers = 100;
constexpr std::size_t kPage = 10;

// --- S1: GET /api/auth/me -----------------------------------------------------------

using user_row = std::tuple<std::int32_t, std::string, std::int32_t, std::optional<std::string>, std::string>;

std::uint64_t touch_user(const user_row& u) {
    const auto& [id, name, group, email, hash] = u;
    return static_cast<std::uint64_t>(id + group) + name.size() + (email ? email->size() : 0) + hash.size();
}

std::uint64_t touch_peer(const std::tuple<std::string, std::int32_t, std::int32_t, std::int32_t>& p) {
    return std::get<0>(p).size() + static_cast<std::uint64_t>(std::get<1>(p) + std::get<2>(p) + std::get<3>(p));
}

std::string token_of(Ctx& x) { return "tok" + std::to_string(x.rng.key(kOjUsers)); }

void me_serial(pg::conn& c, Ctx& x) {
    const auto name = c.query_one<std::string>(pg::sql{"SELECT username FROM oj_sessions WHERE token_hash = {}", token_of(x)});
    const auto u = c.query_one<std::int32_t, std::string, std::int32_t, std::optional<std::string>, std::string>(
        pg::sql{"SELECT id, username, usergroup, email, password_hash FROM oj_users WHERE username = {}", name});
    const auto rating = c.query_one<std::int32_t>(pg::sql{"SELECT rating FROM oj_users WHERE username = {}", name});
    const auto msgs = c.query_one<std::int64_t>(
        pg::sql{"SELECT count(*) FROM oj_msg WHERE receiver = {} AND read_time IS NULL", name});
    const auto sys = c.query_one<std::int64_t>(
        pg::sql{"SELECT count(*) FROM oj_sysmsg WHERE receiver = {} AND read_time IS NULL", name});
    const auto groups = c.query<std::string>(
        pg::sql{"SELECT g.name FROM oj_groups g JOIN oj_group_members m ON m.group_name = g.name "
                "WHERE m.username = {} ORDER BY g.name DESC",
                name});
    const auto admins = c.query_one<std::int64_t>(pg::sql{"SELECT count(*) FROM oj_users WHERE usergroup = 1"});
    const auto peer = c.query_one<std::string, std::int32_t, std::int32_t, std::int32_t>(
        pg::sql{"SELECT username, rating, ac_num, usergroup FROM oj_users WHERE username = ANY({}::text[])",
                "{" + name + "}"});
    if (groups.empty()) throw std::runtime_error("me: no groups");
    x.sink += touch_user(u) + static_cast<std::uint64_t>(rating + msgs + sys + admins) + groups.size() + touch_peer(peer);
}

// q3..q8 for one user, all sent before any is read.
void me_rest_sent(pg::conn& c, Ctx& x, const std::string& name) {
    auto p3 = c.send<std::int32_t>(pg::sql{"SELECT rating FROM oj_users WHERE username = {}", name});
    auto p4 = c.send<std::int64_t>(
        pg::sql{"SELECT count(*) FROM oj_msg WHERE receiver = {} AND read_time IS NULL", name});
    auto p5 = c.send<std::int64_t>(
        pg::sql{"SELECT count(*) FROM oj_sysmsg WHERE receiver = {} AND read_time IS NULL", name});
    auto p6 = c.send<std::string>(
        pg::sql{"SELECT g.name FROM oj_groups g JOIN oj_group_members m ON m.group_name = g.name "
                "WHERE m.username = {} ORDER BY g.name DESC",
                name});
    auto p7 = c.send<std::int64_t>(pg::sql{"SELECT count(*) FROM oj_users WHERE usergroup = 1"});
    auto p8 = c.send<std::string, std::int32_t, std::int32_t, std::int32_t>(
        pg::sql{"SELECT username, rating, ac_num, usergroup FROM oj_users WHERE username = ANY({}::text[])",
                "{" + name + "}"});
    const auto rating = p3.one();
    const auto msgs = p4.one();
    const auto sys = p5.one();
    const auto groups = p6.all();
    const auto admins = p7.one();
    const auto peer = p8.one();
    if (groups.empty()) throw std::runtime_error("me: no groups");
    x.sink += static_cast<std::uint64_t>(rating + msgs + sys + admins) + groups.size() + touch_peer(peer);
}

void me_sent(pg::conn& c, Ctx& x) {
    const auto name = c.query_one<std::string>(pg::sql{"SELECT username FROM oj_sessions WHERE token_hash = {}", token_of(x)});
    const auto u = c.query_one<std::int32_t, std::string, std::int32_t, std::optional<std::string>, std::string>(
        pg::sql{"SELECT id, username, usergroup, email, password_hash FROM oj_users WHERE username = {}", name});
    x.sink += touch_user(u);
    me_rest_sent(c, x, name);
}

void me_join(pg::conn& c, Ctx& x) {
    const auto u = c.query_one<std::int32_t, std::string, std::int32_t, std::optional<std::string>, std::string>(
        pg::sql{"SELECT u.id, u.username, u.usergroup, u.email, u.password_hash FROM oj_sessions s "
                "JOIN oj_users u ON u.username = s.username WHERE s.token_hash = {}",
                token_of(x)});
    x.sink += touch_user(u);
    me_rest_sent(c, x, std::get<1>(u));
}

void me_one(pg::conn& c, Ctx& x) {
    const auto r = c.query_one<std::int32_t, std::string, std::int32_t, std::optional<std::string>, std::string,
                               std::int32_t, std::int32_t, std::int64_t, std::int64_t, std::int64_t,
                               std::optional<std::string>>(
        pg::sql{"SELECT u.id, u.username, u.usergroup, u.email, u.password_hash, u.rating, u.ac_num, "
                "(SELECT count(*) FROM oj_msg WHERE receiver = u.username AND read_time IS NULL), "
                "(SELECT count(*) FROM oj_sysmsg WHERE receiver = u.username AND read_time IS NULL), "
                "(SELECT count(*) FROM oj_users WHERE usergroup = 1), "
                "(SELECT string_agg(g.name, ',' ORDER BY g.name DESC) FROM oj_groups g "
                "JOIN oj_group_members m ON m.group_name = g.name WHERE m.username = u.username) "
                "FROM oj_sessions s JOIN oj_users u ON u.username = s.username WHERE s.token_hash = {}",
                token_of(x)});
    const auto& groups = std::get<10>(r);
    if (!groups) throw std::runtime_error("me: no groups");
    x.sink += static_cast<std::uint64_t>(std::get<0>(r) + std::get<2>(r) + std::get<5>(r) + std::get<6>(r) +
                                         std::get<7>(r) + std::get<8>(r) + std::get<9>(r)) +
              std::get<1>(r).size() + (std::get<3>(r) ? std::get<3>(r)->size() : 0) + std::get<4>(r).size() +
              groups->size();
}

// --- S2: submission list page -------------------------------------------------------

struct page_row {
    std::int64_t id{};
    std::int64_t user_id{};
    std::int32_t problem_id{};
    std::string submitter{};
    money score{};
    pg::timestamptz submit_time{};
    bool accepted{};
    static constexpr std::size_t pg_columns = 7;
    static constexpr std::array<std::string_view, 7> pg_names{"id", "user_id", "problem_id", "submitter",
                                                              "score", "submit_time", "accepted"};
    template <class F>
    void pg_each(F&& f) {
        f(id);
        f(user_id);
        f(problem_id);
        f(submitter);
        f(score);
        f(submit_time);
        f(accepted);
    }
};
static_assert(pg::read_shape<page_row>);

std::uint64_t touch_page_row(const page_row& r) {
    return static_cast<std::uint64_t>(r.id + r.user_id + r.problem_id + r.score.units +
                                      r.submit_time.time_since_epoch().count() + r.accepted) +
           r.submitter.size();
}

pg::sql count_sql(std::int64_t user) {
    return pg::sql{"SELECT count(*) FROM submissions WHERE user_id = {}", user};
}
pg::sql page_sql(std::int64_t user) {
    return pg::sql{"SELECT id, user_id, problem_id, submitter, score, submit_time, accepted FROM submissions "
                   "WHERE user_id = {} ORDER BY id DESC LIMIT 10",
                   user};
}

void list_n1(pg::conn& c, Ctx& x) {
    const std::int64_t user = x.rng.key(kSubUsers);
    const auto total = c.query_one<std::int64_t>(count_sql(user));
    const auto rows = c.query<page_row>(page_sql(user));
    if (rows.size() != kPage) throw std::runtime_error("list: wrong row count");
    x.sink += static_cast<std::uint64_t>(total);
    for (const auto& r : rows) {
        x.sink += touch_page_row(r);
        x.sink += c.query_one<std::string>(pg::sql{"SELECT title FROM oj_problems WHERE id = {}", r.problem_id}).size();
    }
}

void list_batch(pg::conn& c, Ctx& x) {
    const std::int64_t user = x.rng.key(kSubUsers);
    const auto total = c.query_one<std::int64_t>(count_sql(user));
    const auto rows = c.query<page_row>(page_sql(user));
    if (rows.size() != kPage) throw std::runtime_error("list: wrong row count");
    std::string keys = "{";
    for (const auto& r : rows) {
        if (keys.size() > 1) keys += ',';
        keys += std::to_string(r.problem_id);
    }
    keys += '}';
    const auto titles = c.query<std::int32_t, std::string>(
        pg::sql{"SELECT id, title FROM oj_problems WHERE id = ANY({}::int[])", keys});
    if (titles.empty()) throw std::runtime_error("list: no titles");
    x.sink += static_cast<std::uint64_t>(total);
    for (const auto& r : rows) x.sink += touch_page_row(r);
    for (const auto& t : titles) x.sink += static_cast<std::uint64_t>(std::get<0>(t)) + std::get<1>(t).size();
}

void list_join(pg::conn& c, Ctx& x) {
    const std::int64_t user = x.rng.key(kSubUsers);
    auto total = c.send<std::int64_t>(count_sql(user));
    auto page = c.send<std::int64_t, std::int64_t, std::int32_t, std::string, money, pg::timestamptz, bool, std::string>(
        pg::sql{"SELECT s.id, s.user_id, s.problem_id, s.submitter, s.score, s.submit_time, s.accepted, p.title "
                "FROM submissions s JOIN oj_problems p ON p.id = s.problem_id WHERE s.user_id = {} "
                "ORDER BY s.id DESC LIMIT 10",
                user});
    const auto n = total.one();
    const auto rows = page.all();
    if (rows.size() != kPage) throw std::runtime_error("list: wrong row count");
    x.sink += static_cast<std::uint64_t>(n);
    for (const auto& r : rows)
        x.sink += static_cast<std::uint64_t>(std::get<0>(r) + std::get<1>(r) + std::get<2>(r) + std::get<4>(r).units +
                                             std::get<5>(r).time_since_epoch().count() + std::get<6>(r)) +
                  std::get<3>(r).size() + std::get<7>(r).size();
}

// --- S3: judger idle poll -----------------------------------------------------------

const std::array<pg::sql, 5> kProbes = {
    pg::sql{"SELECT id FROM oj_queue WHERE status = 'Waiting' AND hack_of IS NULL ORDER BY id LIMIT 1"},
    pg::sql{"SELECT id FROM oj_custom_tests WHERE judge_time IS NULL ORDER BY id LIMIT 1"},
    pg::sql{"SELECT id FROM oj_queue WHERE status = 'Waiting Rejudge' AND hack_of IS NULL ORDER BY id LIMIT 1"},
    pg::sql{"SELECT id FROM oj_queue WHERE status = 'Judged, Waiting' ORDER BY id LIMIT 1"},
    pg::sql{"SELECT id FROM oj_queue WHERE hack_of IS NOT NULL AND status IN ('Waiting','Waiting Rejudge') "
            "ORDER BY id LIMIT 1"},
};

pg::sql auth_sql(Ctx& x) {
    return pg::sql{"SELECT password_hash FROM oj_judgers WHERE name = {}", "j" + std::to_string(x.rng.key(4))};
}

void poll_serial(pg::conn& c, Ctx& x) {
    x.sink += c.query_one<std::string>(auth_sql(x)).size();
    for (const pg::sql& p : kProbes)
        if (c.query_opt<std::int64_t>(p)) throw std::runtime_error("poll: queue not idle");
}

void poll_sent(pg::conn& c, Ctx& x) {
    auto auth = c.send<std::string>(auth_sql(x));
    std::vector<pg::pending<std::int64_t>> probes;
    for (const pg::sql& p : kProbes) probes.push_back(c.send<std::int64_t>(p));
    x.sink += auth.one().size();
    for (auto& p : probes)
        if (p.opt()) throw std::runtime_error("poll: queue not idle");
}

// --- handlers as scenarios ----------------------------------------------------------

using Handler = void (*)(pg::conn&, Ctx&);

void run_handler(const Cfg& cfg, const char* name, Handler h) {
    for (int size : cfg.pools)
        for (int t : cfg.threads) {
            note(std::string(name) + " pool=" + std::to_string(size) + " threads=" + std::to_string(t));
            pg::pool_options o;
            o.dsn = cfg.dsn;
            o.max_conns = size;
            pg::pool p(o);
            emit(name, t, size, run_point(t, cfg.duration, [&](Ctx& x) {
                     x.loop([&] {
                         pg::conn c(p);
                         h(c, x);
                     });
                 }));
        }
}

#define REAL_HANDLER(fn, name) \
    void run_##fn(const Cfg& cfg) { run_handler(cfg, name, fn); }
REAL_HANDLER(me_serial, "real_me_serial")
REAL_HANDLER(me_sent, "real_me_sent")
REAL_HANDLER(me_join, "real_me_join")
REAL_HANDLER(me_one, "real_me_one")
REAL_HANDLER(list_n1, "real_list_n1")
REAL_HANDLER(list_batch, "real_list_batch")
REAL_HANDLER(list_join, "real_list_join")
REAL_HANDLER(poll_serial, "real_poll_serial")
REAL_HANDLER(poll_sent, "real_poll_sent")
#undef REAL_HANDLER

// 60% /api/auth/me, 30% list page, 10% judger poll.
template <Handler Me, Handler List, Handler Poll>
void mix(pg::conn& c, Ctx& x) {
    const auto pick = x.rng.next() % 100;
    if (pick < 60) Me(c, x);
    else if (pick < 90) List(c, x);
    else Poll(c, x);
}

void run_mix_serial(const Cfg& cfg) { run_handler(cfg, "real_mix_serial", mix<me_serial, list_n1, poll_serial>); }
void run_mix_best(const Cfg& cfg) { run_handler(cfg, "real_mix_best", mix<me_one, list_join, poll_sent>); }

// --- S4: revlm dashboard over the dense table ---------------------------------------

struct request_row {
    std::int64_t id{};
    pg::timestamptz time{};
    std::int64_t user_id{};
    std::int64_t token_id{};
    std::int32_t channel_id{};
    std::string model{};
    std::string endpoint{};
    std::string request_id{};
    std::optional<std::string> response_id{};
    std::int32_t status{};
    std::int32_t input_tokens{};
    std::int32_t output_tokens{};
    std::int32_t cache_read_tokens{};
    std::int32_t latency_ms{};
    money usd{};
    std::optional<std::string> error{};
    static constexpr std::size_t pg_columns = 16;
    static constexpr std::array<std::string_view, 16> pg_names{
        "id",       "time",   "user_id",       "token_id",      "channel_id",    "model",
        "endpoint", "request_id", "response_id", "status",      "input_tokens",  "output_tokens",
        "cache_read_tokens", "latency_ms", "usd", "error"};
    template <class F>
    void pg_each(F&& f) {
        f(id);
        f(time);
        f(user_id);
        f(token_id);
        f(channel_id);
        f(model);
        f(endpoint);
        f(request_id);
        f(response_id);
        f(status);
        f(input_tokens);
        f(output_tokens);
        f(cache_read_tokens);
        f(latency_ms);
        f(usd);
        f(error);
    }
};
static_assert(pg::read_shape<request_row>);

struct hour_total {
    std::int64_t count = 0, in = 0, out = 0, usd = 0;
};
using hours = std::map<std::int64_t, hour_total>;

std::int64_t hour_of(pg::timestamptz t) {
    return std::chrono::duration_cast<std::chrono::hours>(t.time_since_epoch()).count();
}

std::uint64_t touch_hours(const hours& h) {
    std::uint64_t s = 0;
    for (const auto& [k, v] : h) s += static_cast<std::uint64_t>(k + v.count + v.in + v.out + v.usd);
    return s;
}


enum class Form { rows, proj, agg };

void run_dash(const Cfg& cfg, const char* name, Form form, bool day) {
    for (int size : cfg.pools)
        for (int t : cfg.threads) {
            note(std::string(name) + " pool=" + std::to_string(size) + " threads=" + std::to_string(t));
            pg::pool_options o;
            o.dsn = cfg.dsn;
            o.max_conns = size;
            pg::pool p(o);
            std::atomic<std::uint64_t> rows_seen{0}, ops_seen{0};
            Result r = run_point(t, cfg.duration, [&](Ctx& x) {
                std::uint64_t rows = 0, ops = 0;
                x.loop([&] {
                    pg::conn c(p);
                    pg::sql where{" WHERE user_id = {}", x.rng.key(kRvUsers)};
                    if (day) where += pg::sql{" AND time >= '2026-10-29 00:00:00+00'"};
                    hours h;
                    if (form == Form::rows) {
                        pg::sql q{"SELECT id, time, user_id, token_id, channel_id, model, endpoint, request_id, "
                                  "response_id, status, input_tokens, output_tokens, cache_read_tokens, latency_ms, "
                                  "usd, error FROM rv_requests"};
                        q += where;
                        const auto v = c.query<request_row>(q);
                        for (const auto& q : v) {
                            auto& e = h[hour_of(q.time)];
                            ++e.count;
                            e.in += q.input_tokens;
                            e.out += q.output_tokens;
                            e.usd += q.usd.units;
                        }
                        rows += v.size();
                    } else if (form == Form::proj) {
                        pg::sql q{"SELECT time, input_tokens, output_tokens, usd FROM rv_requests"};
                        q += where;
                        const auto v = c.query<pg::timestamptz, std::int32_t, std::int32_t, money>(q);
                        for (const auto& q : v) {
                            auto& e = h[hour_of(std::get<0>(q))];
                            ++e.count;
                            e.in += std::get<1>(q);
                            e.out += std::get<2>(q);
                            e.usd += std::get<3>(q).units;
                        }
                        rows += v.size();
                    } else {
                        pg::sql q{"SELECT date_trunc('hour', time), count(*), sum(input_tokens), sum(output_tokens), "
                                  "sum(usd)::numeric(18,6) FROM rv_requests"};
                        q += where;
                        q += pg::sql{" GROUP BY 1"};
                        const auto v = c.query<pg::timestamptz, std::int64_t, std::int64_t, std::int64_t, money>(q);
                        for (const auto& q : v) {
                            auto& e = h[hour_of(std::get<0>(q))];
                            e.count = std::get<1>(q);
                            e.in = std::get<2>(q);
                            e.out = std::get<3>(q);
                            e.usd = std::get<4>(q).units;
                            rows += 1;
                        }
                    }
                    if (h.empty()) throw std::runtime_error("dash: no rows");
                    x.sink += touch_hours(h);
                    ++ops;
                });
                rows_seen += rows;
                ops_seen += ops;
            });
            const auto n = ops_seen.load();
            if (!r.extra.empty()) r.extra += ';';
            r.extra += "rows=" + std::to_string(n ? rows_seen.load() / n : 0);
            emit(name, t, size, r);
        }
}

void run_dash_day_rows(const Cfg& cfg) { run_dash(cfg, "real_dash_day_rows", Form::rows, true); }
void run_dash_day_proj(const Cfg& cfg) { run_dash(cfg, "real_dash_day_proj", Form::proj, true); }
void run_dash_day_agg(const Cfg& cfg) { run_dash(cfg, "real_dash_day_agg", Form::agg, true); }
void run_dash_all_rows(const Cfg& cfg) { run_dash(cfg, "real_dash_all_rows", Form::rows, false); }
void run_dash_all_proj(const Cfg& cfg) { run_dash(cfg, "real_dash_all_proj", Form::proj, false); }
void run_dash_all_agg(const Cfg& cfg) { run_dash(cfg, "real_dash_all_agg", Form::agg, false); }

}  // namespace

void register_real(std::vector<Scenario>& v) {
    v.push_back({"real_me_serial", run_me_serial});
    v.push_back({"real_me_sent", run_me_sent});
    v.push_back({"real_me_join", run_me_join});
    v.push_back({"real_me_one", run_me_one});
    v.push_back({"real_list_n1", run_list_n1});
    v.push_back({"real_list_batch", run_list_batch});
    v.push_back({"real_list_join", run_list_join});
    v.push_back({"real_poll_serial", run_poll_serial});
    v.push_back({"real_poll_sent", run_poll_sent});
    v.push_back({"real_dash_day_rows", run_dash_day_rows});
    v.push_back({"real_dash_day_proj", run_dash_day_proj});
    v.push_back({"real_dash_day_agg", run_dash_day_agg});
    v.push_back({"real_dash_all_rows", run_dash_all_rows});
    v.push_back({"real_dash_all_proj", run_dash_all_proj});
    v.push_back({"real_dash_all_agg", run_dash_all_agg});
    v.push_back({"real_mix_serial", run_mix_serial});
    v.push_back({"real_mix_best", run_mix_best});
}

}  // namespace bench
