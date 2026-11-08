// Learned read prefetch (docs/adr/0025): the scope of reads on a conn, the
// per-pool table of which reads follow each first read, and the plans built
// from it. Internal to the library.
#pragma once
#include <libpq-fe.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "pg/pool.hpp"

namespace pg::detail {

using shared_result = std::shared_ptr<PGresult>;

// Where a follower's parameter comes from.
struct source {
    enum kind_t : std::uint8_t { root_param, column, literal };
    kind_t kind = literal;
    int index = 0;  // root_param: the root's parameter; column: the parent's column
};

// A result column's type as observed; type_desc views of it point into `name`
// and `column`.
struct col_info {
    oid id = 0;
    std::string name;
    char category = 0;
    char kind = 0;
    std::string column;  // the result column's name
};

// A follower a plan fetches: a read seen after the root with every parameter sourced.
struct planned {
    std::string key;               // its signature
    std::string text;              // its rendered text
    sql tmpl;                      // the statement as observed; literal slots keep their bytes
    std::vector<param> params;     // tmpl's parameters
    std::vector<oid> param_types;  // the types the server resolved for them
    std::vector<source> src;       // one per parameter
    std::vector<col_info> types;   // its result columns as last observed
    int parent = -1;               // -1 the root, else the as-is read in prefetch_plan::reads
    int parent_cols = 0;           // result columns of the parent
    int cols = 0;                  // result columns of the follower
};

// One statement of a plan's batch.
//   root:        the root alone, as the program sent it.
//   plain:       the one member, as is.
//   unified:     the parent with a row number, each member joined laterally to
//                its first max_keys rows; serves the parent too (the root, or an as-is read).
//   partitioned: the members once per distinct value of the parent's `keys` columns.
struct plan_stmt {
    enum kind_t : std::uint8_t { root, plain, unified, partitioned };
    kind_t kind = plain;
    int parent = -1;           // -1 the root, else an as-is read in prefetch_plan::reads
    int parent_cols = 0;
    std::vector<int> members;  // indexes into prefetch_plan::reads
    std::vector<int> keys;     // partitioned: parent columns, ascending
    std::vector<type_desc> cols;  // the result columns it must have; view the plan's col_info
};

// A statement's rendered text and where each of its parameters comes from:
// from[k] >= 0 is the root's parameter from[k], else fixed[k].
struct batch_text {
    std::string text;
    std::vector<int> from;
    std::vector<param> fixed;
};

struct prefetch_plan {
    std::vector<planned> reads;
    std::vector<col_info> root_types;  // the root's result columns
    std::vector<plan_stmt> stmts;  // in batch order; stmts[0] serves the root
    std::size_t root_params = 0;   // parameters of the root template
    std::vector<batch_text> texts; // per statement
    std::vector<batch_text> as_is; // per read; empty for keyed reads
};

// What the pool learned about the reads following one root template.
struct follower_stat {
    planned p;
    std::string parent_key;  // empty for the root
    double hits = 0;         // decayed count of scopes it appeared in
    double rows = 0;         // average rows per execution
    bool failed = false;     // its prefetch statement was refused (read-only, class 42): never planned again
    std::uint64_t paused_until = 0;  // another error: not planned before root_stat::seen reaches it
    bool on = false;         // eligible for the plan as of the last scope
    std::uint64_t id = 0;    // stable while it is in the table
    std::uint64_t since = 0; // root_stat::seen when it was added
};

struct root_stat {
    std::uint64_t seen = 0;  // scopes learned from, not decayed
    double scopes = 0;       // decayed like follower_stat::hits
    double ns = 0;           // average execution time of the root, from unplanned scopes
    double rows = 0;         // average rows of the root
    std::vector<col_info> types;  // the root's result columns as last observed
    bool barren = false;     // seen often, nothing follows it: only sampled
    bool unplannable = false;     // its batch had a syntax error
    bool gate = false;       // the root qualified for a plan as of the last scope
    bool single = false;     // the root had at most one row on average as of the last rebuild
    std::uint64_t sample = 0;
    sql tmpl;                // the root's statement, once a scope recorded it
    std::vector<follower_stat> followers;
    std::uint64_t next_id = 0;
    bool changed = false;    // what the plan is built from changed since it was built
    std::shared_ptr<const prefetch_plan> plan;
};

// The pool's learned table and counters, behind one mutex.
struct prefetch_table {
    std::mutex mu;
    std::unordered_map<std::string, root_stat> roots;
    // A bit per text hash of the roots ever added, read without the lock: a
    // root whose bit is clear is not in the table.
    std::array<std::atomic<std::uint64_t>, 64> known{};
    // Scopes of roots not in the table, by text hash: once a slot counted
    // barren_after of them, its roots are observed only in a sample.
    std::array<std::atomic<std::uint32_t>, 4096> lonely{};
    std::uint64_t plans = 0, hits = 0, misses = 0, unused = 0, failures = 0, skips = 0;
};

// A prefetched result and its column types, which `keep` keeps. `w` is the part
// of `r` that answers the read: a member's rows sit inside the batch result
// they came with.
struct held {
    shared_result r;
    window w;
    std::shared_ptr<const void> keep;
    std::span<const type_desc> cols;
    param_types ptypes;              // of the read it answers
    const planned* read = nullptr;   // the plan's read it answers; null for the root
};

// One candidate read of a scope.
struct trace_entry {
    std::string text;
    sql q;                         // a follower's statement; empty for the root
    small_vec<param, 4> params;
    param_types ptypes;            // resolved by the server; null when unknown
    result own;                    // the result when it came from the server and is a parameter source
    std::shared_ptr<held> from;    // the prefetched result it came from
    const PGresult* r = nullptr;   // the result when it may be a parameter source
    window w;                      // the part of r that is the read's result
    int rows = 0;
    std::shared_ptr<const void> keep;  // keeps cols
    std::span<const type_desc> cols;
    const planned* read = nullptr;     // the plan's read that answered it, while the scope's plan lives
};

// The prefetch statements of a scope in flight (pool.cpp).
struct speculation;

// One statement of a batch; the text lives as long as the batch's `keep`.
struct batch_item {
    std::string_view text;
    std::vector<param> params;
};

// Sends the statements as one round trip on a shared read-only session of the
// pool (opened with a 250 ms statement timeout, up to max(1, max_conns / 4) of
// them); nullptr when none is usable or may be opened.
std::shared_ptr<speculation> speculate(pool_state& st, const pool_options& o, std::vector<batch_item> items,
                                       std::shared_ptr<const void> keep);

// The results of a speculation: results[i] for the statements before `failed`
// (== items.size() when none failed); `failure` is the first error.
struct landed_batch {
    std::vector<result> results;
    std::optional<error> failure;
    std::size_t failed = 0;
};

// Waits for a speculation; never throws an error.
landed_batch land_speculation(speculation& x);

// A read as the scope cache knows it: its text (which the plan owns), its
// parameters, and a hash of both.
struct read_key {
    std::uint64_t hash = 0;
    std::string_view text;
    std::vector<param> params;
};

// A prefetched result waiting for `left` reads of the key.
struct cache_entry {
    read_key key;
    std::shared_ptr<held> h;  // null once served
    int left = 0;
};

// The reads of a conn since its last write, and what the root's plan fetched.
struct prefetch_scope {
    std::vector<trace_entry> trace;
    std::vector<cache_entry> cache;  // oldest first; the entries before `head` are served
    std::size_t head = 0;
    // A large cache is searched through this index: (hash, position in cache), sorted.
    std::vector<std::pair<std::uint64_t, std::uint32_t>> order;
    std::size_t cached = 0;  // reads the cache still answers
    void forget() {
        cache.clear();
        order.clear();
        head = 0;
    }
    std::vector<std::pair<std::string, bool>> failed;  // signatures whose prefetch statement failed; for good?
    std::uint64_t plans = 0, hits = 0, misses = 0, unused = 0, failures = 0, skips = 0;
    bool planned = false;  // the root ran a plan
    bool known = false;    // the root is in the learned table
    bool want_tmpl = false;  // the table wants the root's statement
    std::int64_t root_ns = 0;

    // The root's plan batch in flight, taken at the next read or at the end.
    std::shared_ptr<const prefetch_plan> plan;
    std::vector<param> root_params;
    std::vector<read_key> as_is_keys;  // per plan read: its cache key when it is an as-is read
    std::shared_ptr<held> served;  // the root's result when the plan's batch carried it
    bool stale = false;            // a batch result did not have the planned columns
    bool unplannable = false;      // the batch had a syntax error
    bool timed = false;            // root_ns is the root's own time

    bool active() const { return !trace.empty() || cached || plans || failures || skips; }
    void clear();
};

prefetch_table& prefetch_of(pool_state& st);

// Keeps a cleared scope for the next conn of this thread.
void recycle(std::unique_ptr<prefetch_scope> sc);

// Copies the counters into the stats.
void prefetch_counts(prefetch_table& t, pool_stats& out);

}  // namespace pg::detail
