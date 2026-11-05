// pg_bench: benchmarks of libpg v1 and of raw-libpq prototypes of the planned engine.
//   pg_bench [--dsn DSN] [--duration S] [--threads "1 2 4"] [--pool "8 16"]
//            [--pc-threads "8 16"] [--pc-pool "4 8"] [--list] [SCENARIO...]
//   pg_bench --summarize results.csv        (markdown report on stdout)
// Every measured point prints: scenario,threads,pool,ops_per_sec,p50_us,p99_us,p999_us,extra
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>

#include "harness.hpp"
#include "pg/pg.hpp"

namespace {

using bench::Cfg;
using bench::Scenario;

[[noreturn]] void usage() {
    std::fputs("usage: pg_bench [--dsn DSN] [--duration S] [--threads L] [--pool L] [--pc-threads L] "
               "[--pc-pool L] [--list] [SCENARIO...]\n       pg_bench --summarize results.csv\n",
               stderr);
    std::exit(2);
}

struct Args {
    Cfg cfg;
    std::vector<std::string> names;
    bool list = false;
    std::string summarize;
};

Args parse(int argc, char** argv) {
    Args a;
    a.cfg.dsn = "postgresql://postgres@127.0.0.1:54400/bench";
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        auto value = [&]() -> std::string {
            if (++i >= argc) usage();
            return argv[i];
        };
        if (arg == "--dsn") a.cfg.dsn = value();
        else if (arg == "--duration") a.cfg.duration = std::stod(value());
        else if (arg == "--threads") a.cfg.threads = bench::parse_list(value());
        else if (arg == "--pool") a.cfg.pools = bench::parse_list(value());
        else if (arg == "--pc-threads") a.cfg.pc_threads = bench::parse_list(value());
        else if (arg == "--pc-pool") a.cfg.pc_pools = bench::parse_list(value());
        else if (arg == "--list") a.list = true;
        else if (arg == "--summarize") a.summarize = value();
        else if (arg.starts_with("--")) usage();
        else a.names.push_back(arg);
    }
    return a;
}

bool wanted(const Args& a, const char* name) {
    if (a.names.empty()) return true;
    for (const auto& n : a.names)
        if (n == name) return true;
    return false;
}

// A failing scenario is reported and skipped; the others still run.
int run_all(const Args& a, const std::vector<Scenario>& all) {
    int failures = 0;
    for (const Scenario& s : all) {
        if (!wanted(a, s.name)) continue;
        try {
            s.run(a.cfg);
        } catch (const std::exception& e) {
            std::fprintf(stderr, "ERROR %s: %s\n", s.name, e.what());
            ++failures;
        }
    }
    pg::shutdown();
    return failures ? 1 : 0;
}

}  // namespace

int main(int argc, char** argv) {
    try {
        const Args a = parse(argc, argv);
        if (!a.summarize.empty()) return bench::summarize(a.summarize);
        std::vector<Scenario> all;
        bench::register_raw(all);
        bench::register_v1(all);
        if (a.list) {
            for (const auto& s : all) std::puts(s.name);
            return 0;
        }
        return run_all(a, all);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "pg_bench: %s\n", e.what());
        return 2;
    }
}
