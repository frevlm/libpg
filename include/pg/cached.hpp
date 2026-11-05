#pragma once
#include <chrono>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>

#include "sql.hpp"

namespace pg {

struct cache_options {
    // Period of the background flusher.
    std::chrono::milliseconds interval{100};
    // Called when a write-back fails; empty means print to stderr.
    std::function<void(const pg::error&)> on_error;
};

// May be called before or after the first cached cell is used.
void configure_cache(cache_options);

// Writes every cell that is dirty at the time of the call; returns after commit.
void flush();

// flush(), then stops the flusher thread. Idempotent; a later write starts the
// flusher again. Call it before main returns: nothing is written at process exit
// (a note on stderr says how many cells were lost). Called from an on_error
// handler, i.e. on the flusher thread itself, it only flushes.
void shutdown();

template <class Col>
class cached;

namespace detail {

template <class Col>
inline constexpr bool is_cell<cached<Col>> = true;

// Lock order. Five kinds of locks exist: shutdown_mu (serializes shutdown), flush_mu
// (serializes flushes and loads), the control mutex (dirty list, options, thread
// control), the shard locks of the slot registry and one mutex per slot.
// shutdown_mu is outermost, then flush_mu, then a slot lock. The control mutex
// and the shard locks are leaves: nothing is acquired while holding one, and
// they are never held together. shutdown_mu and flush_mu are held across
// database IO; the other locks never are. User callbacks run with
// none of these held, except update()'s function, which runs under its slot lock.

// Only arithmetic columns (not bool, not optional) have += and delta write-back.
template <class T>
inline constexpr bool has_delta = std::is_arithmetic_v<T> && !std::is_same_v<T, bool>;

// out = a + b / a - b; false on integer overflow. Floating point never overflows.
template <class T>
bool checked_add(T a, T b, T& out) {
    if constexpr (std::is_integral_v<T>) {
        return !__builtin_add_overflow(a, b, &out);
    } else {
        out = static_cast<T>(a + b);
        return true;
    }
}
template <class T>
bool checked_sub(T a, T b, T& out) {
    if constexpr (std::is_integral_v<T>) {
        return !__builtin_sub_overflow(a, b, &out);
    } else {
        out = static_cast<T>(a - b);
        return true;
    }
}

// One UPDATE ready to run: `p` is the value (or delta) followed by the key.
struct op {
    const cell_sql* cell = nullptr;  // immortal
    bool delta = false;              // add `p[0]` to the column instead of assigning it
    params p;

    const std::string& statement() const { return delta ? cell->add : cell->set; }
};

// What a slot has to write back.
template <class T>
struct dirty_state {
    bool set = false;  // assigned: write the memory value
    std::conditional_t<has_delta<T>, T, std::monostate> delta{};  // otherwise: add this
};

// The untyped face of a slot, as the flusher sees it.
class slot_base : public std::enable_shared_from_this<slot_base> {
public:
    explicit slot_base(std::string label) : label_(std::move(label)) {}
    virtual ~slot_base() = default;

    // Under the slot lock: moves the pending write into `out` and clears the
    // dirty state. False when nothing is pending. Leaves the slot untouched when it throws.
    virtual bool take(op& out) = 0;
    // The last taken write was not applied: merges it back and queues the slot
    // again. Without `with_delta` the taken delta may or may not have reached the
    // database, so it is not replayed and the slot reloads on its next access.
    virtual void restore(bool with_delta) = 0;
    // The last taken write failed for good and is discarded; the slot reloads
    // from the database on its next access (unless an assignment is pending).
    virtual void drop() = 0;
    // Whether a write is pending.
    virtual bool has_pending() = 0;

    // "table.column[key,...]", for error messages.
    const std::string& label() const { return label_; }

private:
    std::string label_;
};

// Puts a slot on the dirty list and makes sure the flusher thread runs.
void schedule(std::shared_ptr<slot_base> s);

// The slot of (table, column, key); `make` runs only when there is none yet.
std::shared_ptr<slot_base> find_or_insert(std::string_view table, std::string_view column, const params& key,
                                          const std::function<std::shared_ptr<slot_base>()>& make);

std::string slot_label(std::string_view table, std::string_view column, const params& key);

// The mutex that serializes flushes; loading a slot takes it too.
std::mutex& flush_mutex();

// Starts the flusher thread if none runs (and no shutdown is in progress).
void ensure_flusher();

// Test seam: makes the COMMIT of the next batch behave as if the connection
// dropped right before it (the transaction rolls back) or right after it (the
// transaction is committed but the client never learns).
enum class commit_fault { none, lost_before_commit, lost_after_commit };
void inject_commit_fault(commit_fault f);

// mem and the pending delta moved by `delta` through `op` (checked_add or
// checked_sub); throws pg::error and changes nothing on integer overflow.
template <class T, class Op>
void adjust(T& mem, dirty_state<T>& d, T delta, Op op) {
    T new_mem{}, new_delta = d.delta;
    if (!op(mem, delta, new_mem) || (!d.set && !op(d.delta, delta, new_delta)))
        throw error("pg::cached: integer overflow");
    mem = new_mem;
    d.delta = new_delta;
}

// The memory image of one cell, shared by every cached<Col> that names it.
template <class T>
class slot final : public slot_base {
public:
    slot(std::string label, const cell_sql* stmt, params key)
        : slot_base(std::move(label)), stmt_(stmt), key_(std::move(key)) {}

    T get() {
        std::unique_lock lk(mu_);
        wait_loaded(lk);
        return value_;
    }

    // Runs f(value, dirty_state) under the slot lock, then queues the slot if it has a pending write.
    template <class F>
    void modify(F&& f) {
        std::exception_ptr err;
        bool pend, enqueue;
        {
            std::unique_lock lk(mu_);
            wait_loaded(lk);
            try {
                f(value_, dirty_);
            } catch (...) {
                err = std::current_exception();
            }
            pend = pending();
            enqueue = pend && !std::exchange(queued_, true);
        }
        if (enqueue) schedule(shared_from_this());
        else if (pend) ensure_flusher();
        if (err) std::rethrow_exception(err);
    }

    bool take(op& out) override {
        std::lock_guard lk(mu_);
        op next;
        const bool has = fill(next);  // may throw: nothing is changed yet
        queued_ = false;
        taken_ = std::exchange(dirty_, dirty_state<T>{});
        if (has) out = std::move(next);
        return has;
    }

    void restore(bool with_delta) override {
        bool enqueue;
        {
            std::lock_guard lk(mu_);
            merge_taken(with_delta);
            enqueue = pending() && !std::exchange(queued_, true);
        }
        if (enqueue) schedule(shared_from_this());
    }

    void drop() override {
        std::lock_guard lk(mu_);
        unload();
    }

    bool has_pending() override {
        std::lock_guard lk(mu_);
        return pending();
    }

private:
    // Returns with the slot lock held and the slot loaded.
    void wait_loaded(std::unique_lock<std::mutex>& lk) {
        while (!loaded_) {
            lk.unlock();
            load();
            lk.lock();
        }
    }

    // Reads the database without the slot lock, then installs the value unless
    // another thread was faster. Additions not yet written back are applied on
    // top. The flush mutex is held, so no write is in flight: what the database
    // holds plus the pending delta is the right memory value.
    void load() {
        std::lock_guard flushing(flush_mutex());
        T fresh = fetch();
        std::lock_guard lk(mu_);
        if (loaded_) return;
        if constexpr (has_delta<T>) {
            T sum;
            if (!checked_add(fresh, dirty_.delta, sum)) throw error("pg::cached: integer overflow loading " + label());
            fresh = sum;
        }
        value_ = std::move(fresh);
        loaded_ = true;
    }

    T fetch() const {
        conn c;
        auto rows = c.query_params<T>(stmt_->select, key_);
        if (rows.empty()) throw not_found("no row for " + label());
        return std::move(rows.front());
    }

    // Memory can no longer be trusted; a pending assignment is still the truth.
    void unload() {
        if (!dirty_.set) loaded_ = false;
    }

    static bool pending_in(const dirty_state<T>& d) {
        if constexpr (has_delta<T>) return d.set || d.delta != T{};
        else return d.set;
    }
    bool pending() const { return pending_in(dirty_); }

    // Fills `out` from dirty_; the value is the one in memory right now.
    bool fill(op& out) const {
        if (dirty_.set) {
            out.cell = stmt_;
            out.delta = false;
            out.p = {to_param(value_)};
        } else if constexpr (has_delta<T>) {
            if (dirty_.delta == T{}) return false;
            out.cell = stmt_;
            out.delta = true;
            out.p = {to_param(dirty_.delta)};
        } else {
            return false;
        }
        out.p.insert(out.p.end(), key_.begin(), key_.end());
        return true;
    }

    // An assignment made since the take supersedes the old write; a taken
    // assignment is still the memory value; a taken delta is added back, or
    // given up when its outcome is unknown.
    void merge_taken(bool with_delta) {
        if (dirty_.set) return;
        if (taken_.set) {
            dirty_.set = true;
        } else if constexpr (has_delta<T>) {
            if (taken_.delta == T{}) return;
            if (!with_delta) unload();
            else add_back_taken();
        }
    }

    void add_back_taken() {
        if constexpr (has_delta<T>) {
            T sum;
            // A sum that does not fit is written as the memory value instead.
            if (checked_add(dirty_.delta, taken_.delta, sum)) dirty_.delta = sum;
            else dirty_.set = true;
        }
    }

    const cell_sql* stmt_;
    params key_;
    std::mutex mu_;
    T value_{};
    bool loaded_ = false;
    bool queued_ = false;  // on the dirty list (or about to be)
    dirty_state<T> dirty_;
    dirty_state<T> taken_;  // the write of the last take(); only one flush runs at a time
};

template <class Col>
std::shared_ptr<slot<typename Col::type>> slot_for(const params& pk) {
    using T = typename Col::type;
    auto make = [&]() -> std::shared_ptr<slot_base> {
        return std::make_shared<slot<T>>(slot_label(Col::table::name, Col::name, pk),
                                         &cell_sql_of<Col, typename Col::table>(), pk);
    };
    return std::static_pointer_cast<slot<T>>(find_or_insert(Col::table::name, Col::name, pk, make));
}

// `SELECT "pk1", "pk2" FROM t WHERE "u1" = $1 AND ...`
inline std::string pk_select_sql(std::string_view table, std::span<const std::string_view> pk,
                                 std::span<const std::string_view> by) {
    std::string cols;
    for (std::size_t i = 0; i < pk.size(); ++i) cols += (i ? ", " : "") + quote_ident(pk[i]);
    return "SELECT " + cols + " FROM " + quote_ident(table) + where_clause(by, 1);
}

template <class... K>
params fetch_pk(std::tuple<K...>*, const std::string& statement, const params& by_key) {
    conn c;
    auto rows = c.query_params<K...>(statement, by_key);
    if (rows.empty()) throw not_found("no row for " + statement);
    params p;
    if constexpr (sizeof...(K) == 1) {
        p.push_back(to_param(rows.front()));
    } else {
        std::apply([&](const auto&... v) { (p.push_back(to_param(v)), ...); }, rows.front());
    }
    return p;
}

// The primary key of the row a unique key points at.
template <class Col, class By>
params resolve_pk(const params& by_key) {
    using table = typename Col::table;
    static const std::string statement =
        pk_select_sql(table::name, std::span<const std::string_view>(table::key_columns),
                      std::span<const std::string_view>(By::key_columns));
    return fetch_pk(static_cast<typename table::key*>(nullptr), statement, by_key);
}

}  // namespace detail

// One database cell (table, row, column) used like a variable. Reads come from
// memory; writes change memory and are written back by the flusher thread.
template <class Col>
class cached {
public:
    using value_type = typename Col::type;

    // Locates the row by primary key.
    template <class... A>
        requires detail::key_args_for<typename Col::table::key, A...>
    explicit cached(A&&... key)
        : slot_(detail::slot_for<Col>(detail::key_params<typename Col::table::key>(
              std::index_sequence_for<A...>{}, std::forward<A>(key)...))) {}

    // Locates the row by a unique key: one SELECT finds the primary key.
    template <class By, class... A>
        requires detail::key_args_for<typename By::key, A...>
    cached(By, A&&... key)
        : slot_(detail::slot_for<Col>(detail::resolve_pk<Col, By>(
              detail::key_params<typename By::key>(std::index_sequence_for<A...>{}, std::forward<A>(key)...)))) {}

    cached(const cached&) = default;

    // Assigning one cell to another copies the value, like variables.
    cached& operator=(const cached& other) { return *this = other.get(); }

    value_type get() const { return slot_->get(); }
    operator value_type() const { return get(); }

    cached& operator=(const value_type& v) {
        slot_->modify([&](value_type& mem, auto& d) {
            mem = v;
            d.set = true;
        });
        return *this;
    }

    // Integer overflow throws pg::error and changes nothing.
    cached& operator+=(value_type delta)
        requires detail::has_delta<value_type>
    {
        slot_->modify([&](value_type& mem, auto& d) { detail::adjust(mem, d, delta, detail::checked_add<value_type>); });
        return *this;
    }
    cached& operator-=(value_type delta)
        requires detail::has_delta<value_type>
    {
        slot_->modify([&](value_type& mem, auto& d) { detail::adjust(mem, d, delta, detail::checked_sub<value_type>); });
        return *this;
    }
    cached& operator++()
        requires detail::has_delta<value_type>
    {
        return *this += value_type(1);
    }
    cached& operator--()
        requires detail::has_delta<value_type>
    {
        return *this -= value_type(1);
    }

    // Runs f(value_type&) on a copy under the slot lock; when f returns normally
    // the copy becomes the value and the cell counts as assigned.
    template <class F>
    void update(F f) {
        slot_->modify([&](value_type& mem, auto& d) {
            value_type copy = mem;
            f(copy);
            mem = std::move(copy);
            d.set = true;
        });
    }

private:
    std::shared_ptr<detail::slot<value_type>> slot_;
};

}  // namespace pg
