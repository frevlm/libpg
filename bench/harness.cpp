#include "harness.hpp"

#include <cstdio>
#include <sstream>
#include <stdexcept>
#include <thread>

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
        const std::uint64_t t0 = now_ns();
        ctl.phase.store(1);
        sleep_seconds(duration);
        const std::uint64_t t1 = now_ns();
        ctl.phase.store(2);
        for (auto& t : pool) t.join();
        if (ctl.failed) throw std::runtime_error(ctl.error);
        Result r = merge(ctxs, static_cast<double>(t1 - t0) / 1e9);
        Hist aux;
        for (auto& c : ctxs) aux.merge(c->aux);
        if (aux.count() > 0) {
            char buf[96];
            std::snprintf(buf, sizeof buf, "acquire_p50_us=%.3f;acquire_p99_us=%.3f", aux.quantile_us(0.5),
                          aux.quantile_us(0.99));
            r.extra = buf;
        }
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
