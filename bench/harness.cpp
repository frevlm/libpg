#include "harness.hpp"

#include <libpq-fe.h>
#include <sys/resource.h>

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <thread>

#if __has_include(<valgrind/callgrind.h>)
#include <valgrind/callgrind.h>
#define BENCH_WINDOW_START() CALLGRIND_ZERO_STATS
#define BENCH_WINDOW_END() CALLGRIND_DUMP_STATS_AT("window")
#else
#define BENCH_WINDOW_START()
#define BENCH_WINDOW_END()
#endif

namespace bench {

void pause_briefly() { std::this_thread::sleep_for(std::chrono::microseconds(100)); }

namespace {

void fail(Control& ctl, const std::string& what) {
    if (!ctl.failed.exchange(true)) ctl.error = what;
}

void run_worker(Control& ctl, Ctx& ctx, const Worker& worker) {
    try {
        worker(ctx);
    } catch (const std::exception& e) {
        fail(ctl, e.what());
    }
    if (!ctx.is_ready()) ctl.ready.fetch_add(1);  // never reached ready(): do not hang the runner
}

void sleep_seconds(double s) { std::this_thread::sleep_for(std::chrono::duration<double>(s)); }

// CPU spent in the measured window: this process, user + system over every
// thread; and the server's, read from the cgroup cpu.stat that
// LIBPG_BENCH_SERVER_CPU names (-1 without it).
struct cpu_mark {
    std::int64_t client_us = 0;
    std::int64_t server_us = -1;
};

std::int64_t cgroup_usage_us(const char* path) {
    std::ifstream in(path);
    std::string key;
    std::int64_t v = 0;
    while (in >> key >> v)
        if (key == "usage_usec") return v;
    return -1;
}

cpu_mark cpu_now() {
    rusage u{};
    getrusage(RUSAGE_SELF, &u);
    cpu_mark m;
    m.client_us = (u.ru_utime.tv_sec + u.ru_stime.tv_sec) * 1'000'000LL + u.ru_utime.tv_usec + u.ru_stime.tv_usec;
    if (const char* path = std::getenv("LIBPG_BENCH_SERVER_CPU")) m.server_us = cgroup_usage_us(path);
    return m;
}

// The server's statement totals over the window, from pg_stat_statements, when
// LIBPG_BENCH_PGSS names a DSN: reset at the start, summed at the end. The
// per-statement rows go to stderr.
class pgss_window {
public:
    pgss_window() {
        const char* dsn = std::getenv("LIBPG_BENCH_PGSS");
        if (!dsn) return;
        c_ = PQconnectdb(dsn);
        if (PQstatus(c_) != CONNECTION_OK) {
            PQfinish(c_);
            c_ = nullptr;
            return;
        }
        PQclear(PQexec(c_, "SELECT pg_stat_statements_reset()"));
    }
    ~pgss_window() { PQfinish(c_); }
    pgss_window(const pgss_window&) = delete;
    pgss_window& operator=(const pgss_window&) = delete;

    // "calls=..;exec_ms=..;plan_ms=.." over the window, or empty.
    std::string totals() {
        if (!c_) return {};
        const char* mine = "query NOT LIKE '%pg_stat_statements%'";
        PGresult* rows = PQexec(c_, (std::string("SELECT calls, round(total_exec_time::numeric, 3), "
                                                 "round(total_plan_time::numeric, 3), left(regexp_replace(query, '\\s+', ' ', 'g'), 160) "
                                                 "FROM pg_stat_statements WHERE ") + mine + " ORDER BY total_exec_time DESC LIMIT 12").c_str());
        for (int i = 0; i < PQntuples(rows); ++i)
            std::fprintf(stderr, "pgss %s calls, %s exec ms, %s plan ms: %s\n", PQgetvalue(rows, i, 0),
                         PQgetvalue(rows, i, 1), PQgetvalue(rows, i, 2), PQgetvalue(rows, i, 3));
        PQclear(rows);
        PGresult* r = PQexec(c_, (std::string("SELECT sum(calls), round(sum(total_exec_time)::numeric, 3), "
                                              "round(sum(total_plan_time)::numeric, 3) FROM pg_stat_statements WHERE ") + mine).c_str());
        std::string out;
        if (PQresultStatus(r) == PGRES_TUPLES_OK && PQntuples(r) == 1)
            out = std::string("calls=") + PQgetvalue(r, 0, 0) + ";exec_ms=" + PQgetvalue(r, 0, 1) + ";plan_ms=" + PQgetvalue(r, 0, 2);
        PQclear(r);
        return out;
    }

private:
    PGconn* c_ = nullptr;
};

void add_extra(std::string& extra, const std::string& more) {
    if (more.empty()) return;
    if (!extra.empty()) extra += ';';
    extra += more;
}

// Per operation: the client's and server's CPU microseconds over the window.
std::string cpu_per_op(const cpu_mark& a, const cpu_mark& b, double ops) {
    if (ops <= 0) return {};
    char buf[96];
    std::snprintf(buf, sizeof buf, "ops=%.0f;cli_us=%.2f", ops, static_cast<double>(b.client_us - a.client_us) / ops);
    std::string out = buf;
    if (a.server_us >= 0 && b.server_us >= 0) {
        std::snprintf(buf, sizeof buf, ";srv_us=%.2f", static_cast<double>(b.server_us - a.server_us) / ops);
        out += buf;
    }
    return out;
}

Result merge(std::vector<std::unique_ptr<Ctx>>& ctxs, double seconds) {
    Hist lat;
    std::uint64_t ops = 0;
    for (auto& c : ctxs) {
        lat.merge(c->lat);
        ops += c->ops;
    }
    Result r;
    r.ops_per_sec = static_cast<double>(ops) / seconds;
    r.p50_us = lat.quantile_us(0.50);
    r.p99_us = lat.quantile_us(0.99);
    r.p999_us = lat.quantile_us(0.999);
    return r;
}

}  // namespace

Result run_point(int threads, double duration, const Worker& worker) {
    Control ctl;
    std::vector<std::unique_ptr<Ctx>> ctxs;
    for (int i = 0; i < threads; ++i)
        ctxs.push_back(std::make_unique<Ctx>(i, &ctl, 0x1234abcdULL * static_cast<std::uint64_t>(i + 1) + now_ns()));
    std::vector<std::thread> pool;
    for (int i = 0; i < threads; ++i)
        pool.emplace_back([&, i] { run_worker(ctl, *ctxs[static_cast<std::size_t>(i)], worker); });

    while (ctl.ready.load() < threads) sleep_seconds(0.001);
    ctl.phase.store(ctl.failed ? 2 : 0, std::memory_order_release);
    if (!ctl.failed) {
        sleep_seconds(kWarmupSeconds);
        pgss_window pgss;
        BENCH_WINDOW_START();
        const cpu_mark c0 = cpu_now();
        const std::uint64_t t0 = now_ns();
        ctl.phase.store(1);
        sleep_seconds(duration);
        const std::uint64_t t1 = now_ns();
        ctl.phase.store(2);
        const cpu_mark c1 = cpu_now();
        BENCH_WINDOW_END();
        const std::string server_totals = pgss.totals();
        for (auto& t : pool) t.join();
        if (ctl.failed) throw std::runtime_error(ctl.error);
        const double secs = static_cast<double>(t1 - t0) / 1e9;
        Result r = merge(ctxs, secs);
        Hist aux;
        for (auto& c : ctxs) aux.merge(c->aux);
        if (aux.count() > 0) {
            char buf[96];
            std::snprintf(buf, sizeof buf, "acquire_p50_us=%.3f;acquire_p99_us=%.3f", aux.quantile_us(0.5),
                          aux.quantile_us(0.99));
            r.extra = buf;
        }
        add_extra(r.extra, cpu_per_op(c0, c1, r.ops_per_sec * secs));
        add_extra(r.extra, server_totals);
        return r;
    }
    for (auto& t : pool) t.join();
    throw std::runtime_error(ctl.error);
}

void emit(std::string_view scenario, int threads, int pool, const Result& r) {
    auto num = [](double v, const char* fmt) {
        if (std::isnan(v)) return std::string();
        char buf[48];
        std::snprintf(buf, sizeof buf, fmt, v);
        return std::string(buf);
    };
    std::printf("%.*s,%d,%d,%s,%s,%s,%s,%s\n", static_cast<int>(scenario.size()), scenario.data(), threads, pool,
                num(r.ops_per_sec, "%.1f").c_str(), num(r.p50_us, "%.3f").c_str(), num(r.p99_us, "%.3f").c_str(),
                num(r.p999_us, "%.3f").c_str(), r.extra.c_str());
    std::fflush(stdout);
}

void note(const std::string& s) { std::fprintf(stderr, "%s\n", s.c_str()); }

std::vector<int> parse_list(const std::string& s) {
    std::istringstream in(s);
    std::vector<int> v;
    for (int x; in >> x;) v.push_back(x);
    return v;
}

}  // namespace bench
