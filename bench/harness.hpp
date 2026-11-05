// Measurement harness: per-thread log-linear latency histograms, a start/warmup/stop
// controller, and the CSV line every scenario point prints.
#pragma once
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace bench {

inline constexpr std::int64_t kRows = 100'000;  // accounts rows
inline constexpr double kWarmupSeconds = 1.0;

inline std::uint64_t now_ns() {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

// Log-linear buckets over nanoseconds: exact below 32 ns, then 32 sub-buckets per
// power of two (relative error <= 1.6%).
class Hist {
public:
    static constexpr int kSub = 32;
    static constexpr int kBuckets = kSub + 59 * kSub;

    void add(std::uint64_t ns) { ++b_[index(ns)]; }
    void merge(const Hist& o) {
        for (int i = 0; i < kBuckets; ++i) b_[static_cast<std::size_t>(i)] += o.b_[static_cast<std::size_t>(i)];
    }
    std::uint64_t count() const {
        std::uint64_t n = 0;
        for (auto v : b_) n += v;
        return n;
    }
    // Midpoint of the bucket holding quantile q, in microseconds; NaN when empty.
    double quantile_us(double q) const {
        const std::uint64_t total = count();
        if (total == 0) return std::nan("");
        const auto target = static_cast<std::uint64_t>(std::ceil(q * static_cast<double>(total)));
        std::uint64_t seen = 0;
        for (int i = 0; i < kBuckets; ++i) {
            seen += b_[static_cast<std::size_t>(i)];
            if (seen >= target && seen > 0) return (lower(i) + width(i) / 2.0) / 1000.0;
        }
        return std::nan("");
    }

private:
    static int index(std::uint64_t v) {
        if (v < kSub) return static_cast<int>(v);
        const int e = 63 - std::countl_zero(v);  // >= 5
        const int shift = e - 5;
        return kSub + shift * kSub + static_cast<int>((v >> shift) & (kSub - 1));
    }
    static double lower(int i) {
        if (i < kSub) return i;
        const int j = i - kSub, shift = j / kSub, sub = j % kSub;
        return static_cast<double>(static_cast<std::uint64_t>(kSub + sub) << shift);
    }
    static double width(int i) { return i < kSub ? 1.0 : static_cast<double>(std::uint64_t{1} << ((i - kSub) / kSub)); }

    std::array<std::uint64_t, kBuckets> b_{};
};

// splitmix64: one per thread, no shared state.
struct Rng {
    std::uint64_t s;
    std::uint64_t next() {
        std::uint64_t z = (s += 0x9e3779b97f4a7c15ULL);
        z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
        z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
        return z ^ (z >> 31);
    }
    // Uniform key in [1, n].
    std::int64_t key(std::int64_t n = kRows) { return 1 + static_cast<std::int64_t>(next() % static_cast<std::uint64_t>(n)); }
};

void pause_briefly();  // sleeps ~100 us

// Shared by the runner and its workers. phase: -1 setup, 0 warmup, 1 measuring, 2 stopped.
struct Control {
    std::atomic<int> phase{-1};
    std::atomic<int> ready{0};
    std::atomic<bool> failed{false};
    std::string error;  // written once, under `failed` exchange
};

// What a worker thread sees. Everything in it belongs to that thread alone.
class alignas(64) Ctx {
public:
    Ctx(int tid, Control* c, std::uint64_t seed) : tid(tid), rng{seed}, ctl_(c) {}

    int tid;
    Rng rng;
    Hist lat;          // per-step latency
    Hist aux;          // optional second histogram (scenario defined)
    std::uint64_t ops = 0;
    std::uint64_t sink = 0;  // keeps decoded values alive

    bool running() const { return ctl_->phase.load(std::memory_order_relaxed) < 2; }
    bool measuring() const { return ctl_->phase.load(std::memory_order_relaxed) == 1; }
    void add_aux(std::uint64_t ns) {
        if (measuring()) aux.add(ns);
    }

    // Setup is done: wait until every worker is, and the clock starts.
    void ready() {
        if (ready_) return;
        ready_ = true;
        ctl_->ready.fetch_add(1);
        while (ctl_->phase.load(std::memory_order_acquire) < 0) pause_briefly();
    }
    bool is_ready() const { return ready_; }

    // Runs step() until the stop; step returns how many operations it did (or nothing for 1).
    // The latency of each step is recorded while measuring.
    template <class F>
    void loop(F&& step) {
        ready();
        while (running()) {
            const std::uint64_t t0 = now_ns();
            std::uint64_t n = 1;
            if constexpr (std::is_void_v<decltype(step())>) step();
            else n = step();
            const std::uint64_t t1 = now_ns();
            if (measuring()) {
                lat.add(t1 - t0);
                ops += n;
            }
        }
    }

private:
    bool ready_ = false;
    Control* ctl_;
};

struct Result {
    double ops_per_sec = 0;
    double p50_us = std::nan(""), p99_us = std::nan(""), p999_us = std::nan("");
    std::string extra;  // no commas
};

struct Cfg {
    std::string dsn;
    double duration = 5;
    std::vector<int> threads{1, 2, 4, 8, 16, 32, 64};
    std::vector<int> pools{8, 16, 32};
    std::vector<int> pc_threads{8, 16, 32, 64, 128};
    std::vector<int> pc_pools{4, 8, 16, 32};
};

using Worker = std::function<void(Ctx&)>;

// Runs `threads` workers: setup, 1 s warmup, `duration` s measured. Throws if a worker threw.
Result run_point(int threads, double duration, const Worker& worker);

// Prints one CSV line: scenario,threads,pool,ops_per_sec,p50_us,p99_us,p999_us,extra
void emit(std::string_view scenario, int threads, int pool, const Result& r);

// Progress note on stderr.
void note(const std::string& s);

std::vector<int> parse_list(const std::string& s);

struct Scenario {
    const char* name;
    void (*run)(const Cfg&);
};
void register_raw(std::vector<Scenario>&);
void register_v1(std::vector<Scenario>&);

// pg_bench --summarize FILE: the markdown report of a results CSV on stdout.
int summarize(const std::string& csv_path);

}  // namespace bench
