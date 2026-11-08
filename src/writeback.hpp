// Internals of the write-back cache, shared by writeback.cpp (slots, user
// calls), writeback_writer.cpp (the writer thread) and writeback_listen.cpp
// (the listener thread).
#pragma once
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <random>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>

#include "engine.hpp"
#include "pg/writeback.hpp"

namespace pg::detail {

using wb_clock = std::chrono::steady_clock;

// One (table, column) the cache holds slots of.
struct counter {
    std::string table, column;
    std::vector<std::string> key_columns;
    int scale;                       // decimal scale; -1 for an integer column
    std::atomic<bool> watched{false};
    std::string name() const { return table + "." + column; }
};

// Apart by at least a cache line (128 bytes on Apple silicon), data written by
// different threads does not share one.
inline constexpr std::size_t cache_line = 128;

// A lock for short critical sections: spins, then yields the CPU.
class spin_mutex {
public:
    void lock() noexcept {
        for (int spins = 0;;) {
            if (!locked_.exchange(true, std::memory_order_acquire)) return;
            while (locked_.load(std::memory_order_relaxed)) {
                if (++spins < 128) relax();
                else std::this_thread::yield();
            }
        }
    }
    bool try_lock() noexcept {
        return !locked_.load(std::memory_order_relaxed) && !locked_.exchange(true, std::memory_order_acquire);
    }
    void unlock() noexcept { locked_.store(false, std::memory_order_release); }

private:
    static void relax() noexcept {
#if defined(__aarch64__)
        __asm__ __volatile__("yield");
#elif defined(__x86_64__) || defined(__i386__)
        __builtin_ia32_pause();
#endif
    }
    std::atomic<bool> locked_{false};
};

// The one copy of a cell in this process. value = committed + pending, where
// pending is the sum of this process's recorded, unsettled deltas. Guarded by
// its shard's lock; c, key and shard never change.
struct slot {
    counter* c;
    std::vector<std::string> key;
    std::size_t shard;
    std::int64_t value = 0;
    std::int64_t pending = 0;
    bool loaded = false;  // value holds a committed value read from the database
};

// Key texts packed as key_of packs them, and unpacked.
std::string pack_key(const std::vector<std::string>& key);
std::vector<std::string> unpack_key(std::string_view packed);

// The slot map key: table, column and the packed key texts.
void append_slot_key(std::string& out, std::string_view table, std::string_view column, std::string_view packed);
std::string slot_key(std::string_view table, std::string_view column, const std::vector<std::string>& key);

// Slots in shards, each with its own lock: no lock is global.
class slot_table {
public:
    static constexpr std::size_t shard_count = 64;
    static std::size_t shard_of(std::string_view mk);

    std::unique_lock<spin_mutex> lock(std::size_t shard) { return std::unique_lock(shards_[shard].m); }

    // With the shard locked.
    slot* find(std::size_t shard, std::string_view mk);
    slot& emplace(std::size_t shard, std::string_view mk, counter* c, std::vector<std::string> key);

    // The loaded slots of a counter.
    std::vector<slot*> loaded_of(const counter* c);

private:
    friend class shard_locks;
    struct mk_hash {
        using is_transparent = void;
        std::size_t operator()(std::string_view mk) const noexcept { return std::hash<std::string_view>{}(mk); }
    };
    struct alignas(cache_line) shard {
        spin_mutex m;
        std::unordered_map<std::string, std::unique_ptr<slot>, mk_hash, std::equal_to<>> map;
    };
    std::array<shard, shard_count> shards_;
};

// Holds several shards locked, taken in ascending order. Sorts and
// deduplicates `shards` in place; it must outlive the guard.
class shard_locks {
public:
    shard_locks(slot_table& t, std::vector<std::size_t>& shards);
    ~shard_locks();
    shard_locks(const shard_locks&) = delete;
    shard_locks& operator=(const shard_locks&) = delete;

private:
    slot_table& t_;
    const std::vector<std::size_t>& shards_;
};

struct slot_delta {
    slot* s;
    std::int64_t units;
};

// A submitted posting: its rows and the slots its deltas went to.
struct posting_rec {
    std::vector<wb_row> rows;
    std::vector<slot_delta> deltas;
};

// A read for the writer: load one key (c != nullptr), or reconcile everything.
struct read_req {
    counter* c = nullptr;
    std::vector<std::string> key;
    std::promise<std::int64_t> result;
};

// A notification's keys of one counter; all: every key (TRUNCATE).
struct notified_keys {
    counter* c;
    std::vector<std::vector<std::string>> keys;
    bool all = false;
};

// One column's type: the cast text, and the attributes COPY needs.
struct column_type {
    std::string cast;   // format_type(atttypid, atttypmod), for casts
    oid base = 0;       // the type, or a domain's base type
    char category = 0;  // pg_type.typcategory of base
    char kind = 0;      // pg_type.typtype of base ('e' enum)
};

using column_types = std::map<std::string, column_type>;

struct wb_state {
    wb_state(pool& p_, writeback_options o_);

    pool& p;
    writeback_options o;
    slot_table slots;

    // Counters by "table\0column"; entries are never removed.
    std::mutex reg_m;
    std::map<std::string, std::unique_ptr<counter>> counters;

    // Submitted postings, in shards picked by thread.
    struct alignas(cache_line) queue {
        std::mutex m;
        std::vector<posting_rec> v;
        std::size_t last = 0;  // writer only: postings the last drain took
    };
    static constexpr std::size_t queue_count = 16;
    std::array<queue, queue_count> queues;

    // Counted by every submit, on a cache line of its own.
    alignas(cache_line) std::atomic<std::size_t> backlog{0};
    alignas(cache_line) std::atomic<std::uint64_t> committed{0};
    std::atomic<std::uint64_t> lost{0}, replays{0}, reconciled{0}, full_reconciles{0}, ignored{0};

    // The writer's mailbox.
    std::mutex m;
    std::condition_variable cv;       // wakes the writer
    std::condition_variable done_cv;  // flush progress
    bool wake = false;
    bool stop = false;
    wb_clock::time_point stop_deadline;
    std::uint64_t flush_req = 0, flush_done = 0;
    std::vector<std::shared_ptr<read_req>> reads;
    std::vector<notified_keys> notified;
    bool full_due = false;
    std::atomic<bool> stopping{false};     // stop is set; stop_deadline is valid
    std::atomic<bool> listen_stop{false};  // the listener ends

    // Backend pids of the writer's sessions; their notifications are skipped.
    std::mutex pid_m;
    std::vector<int> own_pids;

    std::atomic<int> fault{0};

    // Writer thread only.
    std::unique_ptr<session> s;
    std::map<std::string, column_types> types;  // table -> column -> type
    std::mt19937_64 rng;
    wb_clock::time_point next_cleanup, next_full;
    std::optional<error> last;

    std::thread writer;
    std::thread listener;
    int wake_pipe[2] = {-1, -1};

    counter& counter_for(const counter_def& d);
    counter* find_counter(std::string_view table, std::string_view column);
    bool own_pid(int pid);
    void post_wake();
};

void writer_loop(wb_state& w);
void listen_loop(wb_state& w);

// Applies deltas to their slots, all shards held at once. committed: the
// delta leaves pending; lost: it leaves value and pending.
enum class settle { committed, lost };
void settle_deltas(wb_state& w, const std::vector<slot_delta>& ds, settle how);

// The contract payload {"t":"table","c":"col","k":[["k1",...],...]}, or
// {"t":"table","c":"col","all":true} when every key changed.
struct notice {
    std::string table, column;
    std::vector<std::vector<std::string>> keys;
    bool all = false;
};
std::optional<notice> parse_notice(std::string_view payload);

// Human-readable text of one array element, for lost postings.
std::string describe_element(const wb_element& e);

}  // namespace pg::detail
