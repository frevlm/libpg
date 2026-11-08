#include "writeback.hpp"

#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <functional>
#include <set>

#include "libpg_schema.hpp"

namespace pg {
namespace detail {

std::string pack_key(const std::vector<std::string>& key) {
    std::string out;
    for (const std::string& k : key) append_key_text(out, k);
    return out;
}

std::vector<std::string> unpack_key(std::string_view packed) {
    std::vector<std::string> out;
    while (!packed.empty()) {
        std::uint32_t n = 0;
        std::memcpy(&n, packed.data(), sizeof n);
        out.emplace_back(packed.substr(sizeof n, n));
        packed.remove_prefix(sizeof n + n);
    }
    return out;
}

void append_slot_key(std::string& out, std::string_view table, std::string_view column, std::string_view packed) {
    out += table;
    out += '\0';
    out += column;
    out += packed;
}

std::string slot_key(std::string_view table, std::string_view column, const std::vector<std::string>& key) {
    std::string mk;
    append_slot_key(mk, table, column, pack_key(key));
    return mk;
}

std::size_t slot_table::shard_of(std::string_view mk) { return mk_hash{}(mk) % shard_count; }

slot* slot_table::find(std::size_t shard, std::string_view mk) {
    auto& map = shards_[shard].map;
    const auto it = map.find(mk);
    return it == map.end() ? nullptr : it->second.get();
}

slot& slot_table::emplace(std::size_t shard, std::string_view mk, counter* c, std::vector<std::string> key) {
    auto& map = shards_[shard].map;
    auto it = map.find(mk);
    if (it == map.end()) it = map.emplace(std::string(mk), std::make_unique<slot>(slot{c, std::move(key), shard})).first;
    return *it->second;
}

std::vector<slot*> slot_table::loaded_of(const counter* c) {
    std::vector<slot*> out;
    for (shard& sh : shards_) {
        std::lock_guard lk(sh.m);
        for (auto& [mk, s] : sh.map)
            if (s->c == c && s->loaded) out.push_back(s.get());
    }
    return out;
}

shard_locks::shard_locks(slot_table& t, std::vector<std::size_t>& shards) : t_(t), shards_(shards) {
    std::sort(shards.begin(), shards.end());
    shards.erase(std::unique(shards.begin(), shards.end()), shards.end());
    for (std::size_t i : shards) t_.shards_[i].m.lock();
}

shard_locks::~shard_locks() {
    for (std::size_t i : shards_) t_.shards_[i].m.unlock();
}

wb_state::wb_state(pool& p_, writeback_options o_) : p(p_), o(std::move(o_)), rng(std::random_device{}()) {
    if (!o.on_lost) {
        o.on_lost = [](const lost_posting& l) {
            std::fprintf(stderr, "pg::writeback: posting lost: %s\n", l.reason.c_str());
            for (const std::string& d : l.description) std::fprintf(stderr, "  %s\n", d.c_str());
        };
    }
    const auto now = wb_clock::now();
    next_cleanup = now;
    next_full = now + o.full_reconcile;
}

counter& wb_state::counter_for(const counter_def& d) {
    std::lock_guard lk(reg_m);
    auto& c = counters[std::string(d.table) + '\0' + std::string(d.column)];
    if (c) return *c;
    c = std::make_unique<counter>();
    c->table = d.table;
    c->column = d.column;
    for (std::string_view k : d.key_columns) c->key_columns.emplace_back(k);
    c->scale = d.scale;
    return *c;
}

counter* wb_state::find_counter(std::string_view table, std::string_view column) {
    std::lock_guard lk(reg_m);
    const auto it = counters.find(std::string(table) + '\0' + std::string(column));
    return it == counters.end() ? nullptr : it->second.get();
}

bool wb_state::own_pid(int pid) {
    std::lock_guard lk(pid_m);
    return std::find(own_pids.begin(), own_pids.end(), pid) != own_pids.end();
}

void wb_state::post_wake() {
    {
        std::lock_guard lk(m);
        wake = true;
    }
    cv.notify_one();
}

void settle_deltas(wb_state& w, const std::vector<slot_delta>& ds, settle how) {
    std::vector<std::size_t> shards;
    for (const slot_delta& d : ds) shards.push_back(d.s->shard);
    const shard_locks held(w.slots, shards);
    for (const slot_delta& d : ds) {
        d.s->pending -= d.units;
        if (how == settle::lost) d.s->value -= d.units;
    }
}

namespace {

// A delta resolved to its slot map key (at, len in scratch::mks), deltas to one
// slot summed.
struct target {
    std::size_t at, len;
    std::size_t shard;
    const wb_delta* d;
    std::int64_t units;
};

// One thread's working space for get and submit, reused so that neither
// allocates once the thread has warmed up.
struct scratch {
    std::string mks;  // slot map keys, back to back
    std::vector<target> ts;
    std::vector<std::size_t> shards;

    std::string_view mk(const target& t) const { return std::string_view(mks).substr(t.at, t.len); }
};

scratch& scratch_of_thread() {
    thread_local scratch sc;
    return sc;
}

void targets_of(scratch& sc, const std::vector<wb_delta>& deltas) {
    sc.mks.clear();
    sc.ts.clear();
    for (const wb_delta& d : deltas) {
        const std::size_t at = sc.mks.size();
        append_slot_key(sc.mks, d.def.table, d.def.column, d.key);
        const std::string_view mk = std::string_view(sc.mks).substr(at);
        auto same = std::find_if(sc.ts.begin(), sc.ts.end(), [&](const target& o) { return sc.mk(o) == mk; });
        if (same != sc.ts.end()) {
            same->units += d.units;
            sc.mks.resize(at);
            continue;
        }
        sc.ts.push_back({at, mk.size(), slot_table::shard_of(mk), &d, d.units});
    }
}

// Adds the deltas to value and pending of their slots (made when missing),
// every shard held at once so a reader sees all of them or none. The counter
// registry is only consulted for a new slot (lock order: shard, then registry).
std::vector<slot_delta> apply_deltas(wb_state& w, const std::vector<wb_delta>& deltas) {
    scratch& sc = scratch_of_thread();
    targets_of(sc, deltas);
    sc.shards.clear();
    for (const target& t : sc.ts) sc.shards.push_back(t.shard);
    std::vector<slot_delta> out;
    out.reserve(sc.ts.size());
    const shard_locks held(w.slots, sc.shards);
    for (const target& t : sc.ts) {
        slot* s = w.slots.find(t.shard, sc.mk(t));
        if (!s) s = &w.slots.emplace(t.shard, sc.mk(t), &w.counter_for(t.d->def), unpack_key(t.d->key));
        s->value += t.units;
        s->pending += t.units;
        out.push_back({s, t.units});
    }
    return out;
}

// Each thread submits to its own queue while there are no more threads than queues.
wb_state::queue& queue_of_thread(wb_state& w) {
    static std::atomic<std::size_t> next{0};
    thread_local const std::size_t i = next.fetch_add(1, std::memory_order_relaxed) % wb_state::queue_count;
    return w.queues[i];
}

}  // namespace

namespace {

std::string two(long v, int width = 2) {
    std::string s = std::to_string(v);
    return std::string(static_cast<std::size_t>(std::max(0, width - static_cast<int>(s.size()))), '0') + s;
}

std::string describe_time(timestamptz t) {
    const auto day = std::chrono::floor<std::chrono::days>(t);
    const std::chrono::year_month_day ymd{day};
    const std::chrono::hh_mm_ss hms{t - day};
    return std::to_string(static_cast<int>(ymd.year())) + "-" + two(static_cast<unsigned>(ymd.month())) + "-" +
           two(static_cast<unsigned>(ymd.day())) + " " + two(hms.hours().count()) + ":" + two(hms.minutes().count()) +
           ":" + two(hms.seconds().count()) + "." + two(hms.subseconds().count(), 6) + "+00";
}

template <class T>
T decoded(oid type, std::string_view b) {
    return codec<T>::decode(b, *builtin_type(type));
}

}  // namespace

std::string describe_element(const wb_element& e) {
    if (!e.bytes) return "NULL";
    const std::string_view b = *e.bytes;
    switch (e.type) {
        case 16: return decoded<bool>(e.type, b) ? "true" : "false";
        case 21: return std::to_string(decoded<std::int16_t>(e.type, b));
        case 23: return std::to_string(decoded<std::int32_t>(e.type, b));
        case 20: return std::to_string(decoded<std::int64_t>(e.type, b));
        case 700: return std::to_string(decoded<float>(e.type, b));
        case 701: return std::to_string(decoded<double>(e.type, b));
        case 1700: return decoded<numeric>(e.type, b).to_string();
        case 2950: return decoded<uuid>(e.type, b).to_string();
        case 1184: return describe_time(decoded<timestamptz>(e.type, b));
        case 1082: return describe_time(decoded<date>(e.type, b)).substr(0, 10);
        case text_oid: return "'" + std::string(b) + "'";
        default: return "<" + std::to_string(b.size()) + " bytes>";
    }
}

void inject_fault(writeback& w, wb_fault f) { w.st_->fault.store(static_cast<int>(f)); }

}  // namespace detail

namespace {

// The column's four triggers must exist and run libpg.watch_notify.
void check_watched(pool& p, const detail::counter_def& d) {
    const std::string base = detail::watch_trigger_base(d.table, d.column);
    std::set<std::string> names;
    {
        conn c(p);
        for (std::string& name : c.query<std::string>(
                 sql{"SELECT t.tgname::text FROM pg_trigger t WHERE t.tgrelid = to_regclass({}) "
                     "AND t.tgfoid = to_regprocedure('libpg.watch_notify()')",
                     detail::quote_ident(d.table)}))
            names.insert(std::move(name));
    }
    for (const char* suffix : {"_ins", "_upd", "_del", "_trn"})
        if (!names.contains(base + suffix))
            throw error(error_kind::other, "writeback: " + std::string(d.table) + "." + std::string(d.column) +
                                               " is not watched; add pg::watch<Col>() to migrate_options::watched and run pg::migrate");
}

}  // namespace

writeback::writeback(pool& p, writeback_options o) : st_(std::make_unique<detail::wb_state>(p, std::move(o))) {
    if (pipe(st_->wake_pipe) != 0) throw error(error_kind::other, "writeback: pipe() failed");
    st_->writer = std::thread(detail::writer_loop, std::ref(*st_));
}

writeback::~writeback() {
    detail::wb_state& w = *st_;
    if (w.listener.joinable()) {
        w.listen_stop.store(true);
        const char b = 0;
        [[maybe_unused]] auto n = write(w.wake_pipe[1], &b, 1);
        w.listener.join();
    }
    {
        std::lock_guard lk(w.m);
        w.stop = true;
        w.stop_deadline = detail::wb_clock::now() + w.o.shutdown_timeout;
        w.stopping.store(true);
    }
    w.cv.notify_one();
    w.writer.join();
    close(w.wake_pipe[0]);
    close(w.wake_pipe[1]);
}

void writeback::watch_counter(const detail::counter_def& d) {
    detail::wb_state& w = *st_;
    check_watched(w.p, d);
    w.counter_for(d).watched.store(true);
    std::lock_guard lk(w.reg_m);
    if (!w.listener.joinable()) w.listener = std::thread(detail::listen_loop, std::ref(w));
}

std::int64_t writeback::value(const detail::counter_def& d, const std::string& key) {
    detail::wb_state& w = *st_;
    std::string& mk = detail::scratch_of_thread().mks;
    mk.clear();
    detail::append_slot_key(mk, d.table, d.column, key);
    const std::size_t shard = detail::slot_table::shard_of(mk);
    {
        const auto lk = w.slots.lock(shard);
        const detail::slot* s = w.slots.find(shard, mk);
        if (s && s->loaded) return s->value;
    }
    auto req = std::make_shared<detail::read_req>();
    req->c = &w.counter_for(d);
    req->key = detail::unpack_key(key);
    auto result = req->result.get_future();
    {
        std::lock_guard lk(w.m);
        w.reads.push_back(req);
        w.wake = true;
    }
    w.cv.notify_one();
    return result.get();
}

void writeback::submit(posting p) {
    detail::wb_state& w = *st_;
    if (w.backlog.fetch_add(1) >= w.o.backlog_limit) {
        w.backlog.fetch_sub(1);
        throw backlog_full("writeback backlog is full (" + std::to_string(w.o.backlog_limit) + " postings)");
    }
    detail::posting_rec rec;
    rec.rows = std::move(p.rows_);
    try {
        rec.deltas = detail::apply_deltas(w, p.deltas_);
    } catch (...) {
        w.backlog.fetch_sub(1);
        throw;
    }
    auto& q = detail::queue_of_thread(w);
    std::lock_guard lk(q.m);
    q.v.push_back(std::move(rec));
}

void writeback::flush() {
    detail::wb_state& w = *st_;
    std::unique_lock lk(w.m);
    const std::uint64_t ticket = ++w.flush_req;
    w.wake = true;
    w.cv.notify_one();
    w.done_cv.wait(lk, [&] { return w.flush_done >= ticket; });
}

void writeback::reconcile() {
    detail::wb_state& w = *st_;
    auto req = std::make_shared<detail::read_req>();
    auto result = req->result.get_future();
    {
        std::lock_guard lk(w.m);
        w.reads.push_back(req);
        w.wake = true;
    }
    w.cv.notify_one();
    static_cast<void>(result.get());  // rethrows a failed reconcile
}

writeback_stats writeback::stats() const {
    const detail::wb_state& w = *st_;
    return {w.backlog.load(),  w.committed.load(),       w.lost.load(),   w.replays.load(),
            w.reconciled.load(), w.full_reconciles.load(), w.ignored.load()};
}

}  // namespace pg
