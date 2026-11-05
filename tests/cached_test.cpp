#include <catch2/catch_test_macros.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <limits>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "pg/pg.hpp"

using namespace std::chrono_literals;

// Hand-written tags in the shape pg_tablegen generates.
namespace db::cache_cells {
struct table_ {
    static constexpr std::string_view name = "cache_cells";
    using key = std::tuple<std::int32_t>;
    static constexpr std::array<std::string_view, 1> key_columns{"id"};
};
struct n {
    using table = table_;
    using type = std::int64_t;
    static constexpr std::string_view name = "n";
};
struct label {
    using table = table_;
    using type = std::optional<std::string>;
    static constexpr std::string_view name = "label";
};
struct price {
    using table = table_;
    using type = pg::numeric;
    static constexpr std::string_view name = "price";
};
struct ord {
    using table = table_;
    using type = std::int32_t;
    static constexpr std::string_view name = "ord";
};
struct by_code_t {
    using table = table_;
    using key = std::tuple<std::string>;
    static constexpr std::array<std::string_view, 1> key_columns{"code"};
};
inline constexpr by_code_t by_code{};
}  // namespace db::cache_cells

namespace db::cache_grid {
struct table_ {
    static constexpr std::string_view name = "cache_grid";
    using key = std::tuple<std::int32_t, std::string>;
    static constexpr std::array<std::string_view, 2> key_columns{"a", "b"};
};
struct v {
    using table = table_;
    using type = std::int64_t;
    static constexpr std::string_view name = "v";
};
struct note {
    using table = table_;
    using type = std::optional<std::string>;
    static constexpr std::string_view name = "note";
};
}  // namespace db::cache_grid

namespace {

namespace cells = db::cache_cells;

// Collects what the flusher reports: "NF" for pg::not_found, else the SQLSTATE.
struct errors {
    std::mutex mu;
    std::vector<std::string> seen;
    int count() {
        std::lock_guard lk(mu);
        return static_cast<int>(seen.size());
    }
    std::vector<std::string> all() {
        std::lock_guard lk(mu);
        return seen;
    }
};

std::string dsn() {
    const char* d = std::getenv("LIBPG_TEST_DSN");
    REQUIRE(d != nullptr);
    return d;
}

// Stops the flusher, applies the options (the interval defaults to "never" so
// that tests decide when to flush) and routes errors into `e`.
void configure(errors& e, std::chrono::milliseconds interval = 1h) {
    pg::configure_cache({1h, nullptr});  // the previous test's collector may be gone
    pg::shutdown();
    pg::configure_cache({interval, [&e](const pg::error& err) {
                             std::lock_guard lk(e.mu);
                             e.seen.push_back(dynamic_cast<const pg::not_found*>(&err) ? "NF" : err.sqlstate);
                         }});
}

// Recreates the table: ids 1..60 with n = 70 and code 'c<id>'. `writes` counts
// the UPDATE statements that touched a row.
void reset() {
    pg::connect(dsn(), 8);
    pg::conn c;
    c.exec("DROP TABLE IF EXISTS cache_cells");
    c.exec(R"sql(CREATE TABLE cache_cells (
        id int PRIMARY KEY, code text NOT NULL UNIQUE,
        n bigint NOT NULL DEFAULT 0 CHECK (n >= 0), writes int NOT NULL DEFAULT 0,
        label text, price numeric NOT NULL DEFAULT 0,
        ord int NOT NULL UNIQUE DEFERRABLE INITIALLY DEFERRED
    ))sql");
    c.exec(R"sql(CREATE OR REPLACE FUNCTION cache_cells_count() RETURNS trigger AS $$
        BEGIN NEW.writes := OLD.writes + 1; RETURN NEW; END $$ LANGUAGE plpgsql)sql");
    c.exec("CREATE TRIGGER count_writes BEFORE UPDATE ON cache_cells FOR EACH ROW EXECUTE FUNCTION cache_cells_count()");
    c.exec("INSERT INTO cache_cells (id, code, n, ord) SELECT i, 'c' || i, 70, i FROM generate_series(1, 60) i");
}

std::int64_t db_n(int id) {
    pg::conn c;
    return c.query_one<std::int64_t>("SELECT n FROM cache_cells WHERE id = $1", id).value();
}

std::int32_t db_writes(int id) {
    pg::conn c;
    return c.query_one<std::int32_t>("SELECT writes FROM cache_cells WHERE id = $1", id).value();
}

}  // namespace

template <class S>
concept has_arithmetic = requires(S s, typename S::value_type v) {
    s += v;
    s -= v;
    ++s;
    --s;
};

static_assert(has_arithmetic<pg::cached<cells::n>>);
static_assert(!has_arithmetic<pg::cached<cells::label>>);
static_assert(!has_arithmetic<pg::cached<cells::price>>);

TEST_CASE("reads are served from memory after the first load") {
    reset();
    errors e;
    configure(e);
    pg::cached<cells::n> x{1};
    REQUIRE(x.get() == 70);
    pg::conn{}.exec("UPDATE cache_cells SET n = 999 WHERE id = 1");
    REQUIRE(db_n(1) == 999);
    REQUIRE(x.get() == 70);
    std::int64_t as_value = x;
    REQUIRE(as_value == 70);
    REQUIRE(pg::cached<cells::n>{1}.get() == 70);
}

TEST_CASE("cached instances of one cell share memory") {
    reset();
    errors e;
    configure(e);
    pg::cached<cells::n> a{2};
    pg::cached<cells::n> b{2};
    a = 5;
    REQUIRE(b.get() == 5);
    b += 10;
    REQUIRE(a.get() == 15);
    REQUIRE(db_n(2) == 70);
    pg::flush();
    REQUIRE(db_n(2) == 15);
    REQUIRE(e.count() == 0);
}

TEST_CASE("assigning one cell to another copies the value") {
    reset();
    errors e;
    configure(e);
    pg::cached<cells::n> a{3};
    pg::cached<cells::n> b{4};
    a = 11;
    b = a;
    a = 12;
    REQUIRE(b.get() == 11);
    pg::flush();
    REQUIRE(db_n(4) == 11);
}

TEST_CASE("1000 assignments become one UPDATE with the final value") {
    reset();
    errors e;
    configure(e);
    pg::cached<cells::n> x{5};
    for (int i = 1; i <= 1000; ++i) x = i;
    REQUIRE(db_writes(5) == 0);
    pg::flush();
    REQUIRE(db_n(5) == 1000);
    REQUIRE(db_writes(5) == 1);
    pg::flush();
    REQUIRE(db_writes(5) == 1);
    REQUIRE(e.count() == 0);
}

TEST_CASE("8 threads adding 1000 times each reach base + 8000, with the flusher running") {
    reset();
    errors e;
    configure(e, 1ms);
    std::vector<std::thread> threads;
    for (int t = 0; t < 8; ++t)
        threads.emplace_back([] {
            pg::cached<cells::n> x{6};
            for (int i = 0; i < 1000; ++i) x += 1;
        });
    for (auto& t : threads) t.join();
    pg::flush();
    REQUIRE(db_n(6) == 70 + 8000);
    REQUIRE(pg::cached<cells::n>{6}.get() == 70 + 8000);
    REQUIRE(e.count() == 0);
}

TEST_CASE("a delta write-back keeps another connection's additions") {
    reset();
    errors e;
    configure(e, 1ms);
    pg::cached<cells::n> x{7};
    REQUIRE(x.get() == 70);
    std::thread other([] {
        for (int i = 0; i < 200; ++i) pg::conn{}.exec("UPDATE cache_cells SET n = n + 5 WHERE id = 7");
    });
    for (int i = 0; i < 1000; ++i) ++x;
    other.join();
    pg::flush();
    REQUIRE(db_n(7) == 70 + 1000 + 200 * 5);
    REQUIRE(e.count() == 0);
}

TEST_CASE("an assignment after a delta writes the memory value") {
    reset();
    errors e;
    configure(e);
    pg::cached<cells::n> x{8};
    x += 5;
    x = 10;
    pg::flush();
    REQUIRE(db_n(8) == 10);
    REQUIRE(x.get() == 10);
    x += 3;
    x -= 1;
    pg::flush();
    REQUIRE(db_n(8) == 12);
    REQUIRE(x.get() == 12);
    REQUIRE(e.count() == 0);
}

TEST_CASE("an addition before any read applies to the loaded base") {
    reset();
    errors e;
    configure(e);
    pg::cached<cells::n> x{9};
    x += 5;
    REQUIRE(x.get() == 75);
    pg::flush();
    REQUIRE(db_n(9) == 75);
}

TEST_CASE("update runs the function on the memory value and writes it back") {
    reset();
    errors e;
    configure(e);
    pg::cached<cells::label> label{10};
    REQUIRE(label.get() == std::nullopt);
    label.update([](std::optional<std::string>& v) { v = "hello"; });
    REQUIRE(label.get() == std::optional<std::string>{"hello"});
    pg::flush();
    pg::conn c;
    auto row = c.query_one<std::optional<std::string>>("SELECT label FROM cache_cells WHERE id = 10");
    REQUIRE(row.value() == std::optional<std::string>{"hello"});
}

const std::string unreachable_dsn = "postgresql://postgres@127.0.0.1:1/none";

// Waits until `done()` holds or two seconds pass.
template <class F>
bool wait_for(F done) {
    for (int i = 0; i < 400 && !done(); ++i) std::this_thread::sleep_for(5ms);
    return done();
}

TEST_CASE("a permanently failing write is reported, dropped, and the cell reloads") {
    reset();
    errors e;
    configure(e);
    pg::cached<cells::n> x{11};
    x = -1;
    REQUIRE_THROWS_AS(pg::flush(), pg::error);
    REQUIRE(db_n(11) == 70);
    REQUIRE(x.get() == 70);
    x = 5;
    pg::flush();
    REQUIRE(db_n(11) == 5);
    REQUIRE(e.count() == 0);
}

TEST_CASE("a dropped delta is not retried; the cell reloads and later additions apply to the database value") {
    reset();
    errors e;
    configure(e);
    pg::cached<cells::n> x{12};
    x += -100;
    REQUIRE_THROWS_AS(pg::flush(), pg::error);
    REQUIRE(db_n(12) == 70);
    pg::conn{}.exec("UPDATE cache_cells SET n = 500 WHERE id = 12");
    REQUIRE(x.get() == 500);
    x += 200;
    pg::flush();
    REQUIRE(db_n(12) == 700);
    REQUIRE(x.get() == 700);
    REQUIRE(e.count() == 0);
}

TEST_CASE("flush throws the first error of that flush") {
    reset();
    errors e;
    configure(e);
    pg::cached<cells::n> x{30};
    x = -1;
    try {
        pg::flush();
        FAIL("flush did not throw");
    } catch (const pg::error& err) {
        REQUIRE(err.sqlstate == "23514");
    }
    REQUIRE(e.count() == 0);
    pg::flush();
}

TEST_CASE("a poison write does not block the other cells of its batch") {
    reset();
    errors e;
    configure(e);
    pg::cached<cells::n> bad{21};
    pg::cached<cells::n> good_set{22};
    pg::cached<cells::n> good_add{23};
    bad = -1;
    good_set = 222;
    good_add += 5;
    REQUIRE_THROWS_AS(pg::flush(), pg::error);
    REQUIRE(db_n(21) == 70);
    REQUIRE(db_n(22) == 222);
    REQUIRE(db_n(23) == 75);
    pg::flush();
    REQUIRE(db_writes(21) == 0);
    REQUIRE(db_writes(22) == 1);
    REQUIRE(db_writes(23) == 1);
    REQUIRE(e.count() == 0);
}

TEST_CASE("the flusher reports a poison write once and keeps writing the others") {
    reset();
    errors e;
    configure(e, 5ms);
    pg::cached<cells::n> bad{24};
    pg::cached<cells::n> good_set{25};
    pg::cached<cells::n> good_add{26};
    bad = -1;
    good_set = 222;
    good_add += 5;
    REQUIRE(wait_for([&] { return db_n(25) == 222 && db_n(26) == 75; }));
    REQUIRE(wait_for([&] { return e.count() > 0; }));
    std::this_thread::sleep_for(100ms);
    REQUIRE(e.all() == std::vector<std::string>{"23514"});
    REQUIRE(db_writes(24) == 0);
}

TEST_CASE("integer overflow throws and leaves memory and the pending delta unchanged") {
    constexpr std::int64_t max = std::numeric_limits<std::int64_t>::max();
    reset();
    errors e;
    configure(e);
    pg::cached<cells::n> x{27};
    REQUIRE_THROWS_AS(x += max, pg::error);
    REQUIRE_THROWS_AS(++(x = max), pg::error);
    REQUIRE(x.get() == max);
    x = 70;
    REQUIRE_THROWS_AS(x -= -max, pg::error);
    REQUIRE(x.get() == 70);
    pg::flush();
    REQUIRE(db_n(27) == 70);
    REQUIRE(e.count() == 0);
}

TEST_CASE("an overflowing pending delta throws even when the memory value fits") {
    constexpr std::int64_t max = std::numeric_limits<std::int64_t>::max();
    reset();
    errors e;
    configure(e);
    pg::cached<cells::n> x{28};
    x -= max;  // memory 70 - max, delta -max
    REQUIRE_THROWS_AS(x -= 50, pg::error);
    REQUIRE(x.get() == 70 - max);
    x += max;  // delta back to zero: nothing to write
    REQUIRE(x.get() == 70);
    pg::flush();
    REQUIRE(db_writes(28) == 0);
}

TEST_CASE("a transient failure restores the batch and a later flush applies each delta once") {
    reset();
    errors e;
    configure(e);
    pg::cached<cells::n> add{34};
    pg::cached<cells::n> set{35};
    REQUIRE(add.get() == 70);
    REQUIRE(set.get() == 70);
    add += 3;
    set = 9;
    pg::connect(unreachable_dsn, 2);
    REQUIRE_THROWS_AS(pg::flush(), pg::error);
    add += 4;
    pg::connect(dsn(), 8);
    pg::flush();
    REQUIRE(db_n(34) == 77);
    REQUIRE(db_n(35) == 9);
    REQUIRE(add.get() == 77);
    pg::flush();
    REQUIRE(db_writes(34) == 1);
    REQUIRE(e.count() == 0);
}

TEST_CASE("the flusher retries a transient failure by itself") {
    reset();
    errors e;
    configure(e, 5ms);
    pg::cached<cells::n> x{13};
    REQUIRE(x.get() == 70);
    pg::connect(unreachable_dsn, 2);
    x = 8;
    REQUIRE(wait_for([&] { return e.count() >= 2; }));
    pg::connect(dsn(), 8);
    REQUIRE(wait_for([&] { return db_n(13) == 8; }));
}

TEST_CASE("a COMMIT whose outcome is lost after the commit does not apply the delta twice") {
    reset();
    errors e;
    configure(e);
    pg::cached<cells::n> x{32};
    x += 5;
    pg::detail::inject_commit_fault(pg::detail::commit_fault::lost_after_commit);
    pg::flush();
    REQUIRE(db_n(32) == 75);
    pg::flush();
    REQUIRE(db_n(32) == 75);
    REQUIRE(db_writes(32) == 1);
    REQUIRE(x.get() == 75);
    REQUIRE(e.count() == 0);
}

TEST_CASE("a COMMIT lost before the commit is retried once") {
    reset();
    errors e;
    configure(e);
    pg::cached<cells::n> x{33};
    x += 5;
    pg::detail::inject_commit_fault(pg::detail::commit_fault::lost_before_commit);
    REQUIRE_THROWS_AS(pg::flush(), pg::error);
    REQUIRE(db_n(33) == 70);
    pg::flush();
    REQUIRE(db_n(33) == 75);
    pg::flush();
    REQUIRE(db_n(33) == 75);
    REQUIRE(e.count() == 0);
}

TEST_CASE("an update that matches no row is reported once and does not block the batch") {
    reset();
    errors e;
    configure(e);
    pg::cached<cells::n> gone{14};
    pg::cached<cells::n> kept{15};
    pg::cached<cells::n> counted{16};
    REQUIRE(gone.get() == 70);
    pg::conn{}.exec("DELETE FROM cache_cells WHERE id = 14");
    gone = 1;
    kept = 2;
    counted += 3;
    REQUIRE_THROWS_AS(pg::flush(), pg::not_found);
    REQUIRE(db_n(15) == 2);
    REQUIRE(db_n(16) == 73);
    pg::flush();
    REQUIRE(e.count() == 0);
}

TEST_CASE("a unique key shares the slot of the primary key") {
    reset();
    errors e;
    configure(e);
    pg::cached<cells::n> by_pk{17};
    pg::cached<cells::n> by_code{cells::by_code, "c17"};
    by_pk = 42;
    REQUIRE(by_code.get() == 42);
    ++by_code;
    REQUIRE(by_pk.get() == 43);
    pg::flush();
    REQUIRE(db_n(17) == 43);
    REQUIRE_THROWS_AS((pg::cached<cells::n>{cells::by_code, "nope"}), pg::not_found);
}

TEST_CASE("a missing row is not_found") {
    reset();
    errors e;
    configure(e);
    pg::cached<cells::n> x{999};
    REQUIRE_THROWS_AS(x.get(), pg::not_found);
    REQUIRE_THROWS_AS(x = 1, pg::not_found);
    REQUIRE_THROWS_AS(x += 1, pg::not_found);
    pg::flush();
    REQUIRE(e.count() == 0);
}

TEST_CASE("shutdown flushes pending writes and is idempotent") {
    reset();
    errors e;
    configure(e);
    pg::cached<cells::n> x{18};
    x = 9;
    REQUIRE(db_n(18) == 70);
    pg::shutdown();
    REQUIRE(db_n(18) == 9);
    pg::shutdown();
    x = 11;
    pg::flush();
    REQUIRE(db_n(18) == 11);
    REQUIRE(e.count() == 0);
}

TEST_CASE("the flusher writes back by itself after the interval") {
    reset();
    errors e;
    configure(e, 10ms);
    pg::cached<cells::n> x{19};
    x = 21;
    for (int i = 0; i < 400 && db_n(19) != 21; ++i) std::this_thread::sleep_for(5ms);
    REQUIRE(db_n(19) == 21);
    REQUIRE(db_writes(19) == 1);
}

TEST_CASE("configure_cache after the flusher started changes the interval") {
    reset();
    errors e;
    configure(e, 1h);
    pg::cached<cells::n> x{20};
    x = 1;
    pg::configure_cache({10ms, nullptr});
    for (int i = 0; i < 400 && db_n(20) != 1; ++i) std::this_thread::sleep_for(5ms);
    REQUIRE(db_n(20) == 1);
}

TEST_CASE("an on_error handler may call flush and shutdown; later writes still reach the database") {
    reset();
    errors e;
    configure(e, 5ms);
    std::atomic<bool> handled{false};
    pg::configure_cache({5ms, [&](const pg::error&) {
                             try {
                                 pg::flush();
                                 pg::shutdown();
                             } catch (...) {
                             }
                             std::this_thread::sleep_for(50ms);
                             handled = true;
                         }});
    pg::cached<cells::n> bad{31};
    pg::cached<cells::n> later{44};
    bad = -1;
    REQUIRE(wait_for([&] { return handled.load(); }));
    later = 5;
    REQUIRE(wait_for([&] { return db_n(44) == 5; }));
    later = 6;
    REQUIRE(wait_for([&] { return db_n(44) == 6; }));
}

TEST_CASE("the flusher starts again for a write after shutdown") {
    reset();
    errors e;
    configure(e, 10ms);
    pg::cached<cells::n> x{36};
    x = 1;
    pg::shutdown();
    x = 2;
    REQUIRE(wait_for([&] { return db_n(36) == 2; }));
}

TEST_CASE("concurrent shutdowns do not hang") {
    reset();
    errors e;
    configure(e, 1ms);
    std::mutex mu;
    std::condition_variable cv;
    bool finished = false;
    std::thread watchdog([&] {
        std::unique_lock lk(mu);
        if (!cv.wait_for(lk, 120s, [&] { return finished; })) std::abort();
    });
    pg::cached<cells::n> x{37};
    for (int i = 0; i < 200; ++i) {
        x += 1;
        std::this_thread::sleep_for(std::chrono::microseconds(i % 7 * 300));
        std::thread a([] { pg::shutdown(); });
        std::thread b([] { pg::shutdown(); });
        a.join();
        b.join();
    }
    pg::flush();
    REQUIRE(db_n(37) == 70 + 200);
    {
        std::lock_guard lk(mu);
        finished = true;
    }
    cv.notify_all();
    watchdog.join();
}

TEST_CASE("an op the client cannot encode is dropped while the rest of the batch commits") {
    reset();
    errors e;
    configure(e);
    pg::cached<cells::label> bad{45};
    pg::cached<cells::n> other{46};
    REQUIRE(bad.get() == std::nullopt);
    REQUIRE(other.get() == 70);
    bad = std::optional<std::string>{std::string("a\0b", 3)};
    other += 5;
    REQUIRE_THROWS_AS(pg::flush(), pg::error);
    REQUIRE(db_n(46) == 75);
    pg::flush();
    REQUIRE(bad.get() == std::nullopt);
    REQUIRE(e.count() == 0);
}

TEST_CASE("a deferred constraint violation is dropped while the rest of the batch commits") {
    reset();
    errors e;
    configure(e);
    pg::cached<cells::ord> bad{47};
    pg::cached<cells::n> other{48};
    REQUIRE(bad.get() == 47);
    REQUIRE(other.get() == 70);
    bad = 48;  // duplicates the ord of row 48
    other += 7;
    try {
        pg::flush();
        FAIL("flush did not throw");
    } catch (const pg::error& err) {
        REQUIRE(err.sqlstate == "23505");
    }
    REQUIRE(db_n(48) == 77);
    pg::flush();
    REQUIRE(bad.get() == 47);
    REQUIRE(e.count() == 0);
}

// Polls until some backend waits for a lock.
bool lock_wait_seen() {
    return wait_for([] {
        pg::conn c;
        return c.query_one<std::int64_t>(
                    "SELECT count(*) FROM pg_stat_activity WHERE wait_event_type = 'Lock' AND datname = current_database()")
                   .value() > 0;
    });
}

TEST_CASE("a reload waits for the write in flight, so memory ends equal to the database") {
    reset();
    errors e;
    configure(e);
    pg::cached<cells::n> x{41};
    REQUIRE(x.get() == 70);
    pg::conn locker;
    locker.exec("BEGIN");
    locker.exec("UPDATE cache_cells SET n = 30 WHERE id = 41");
    x += -100;
    std::atomic<bool> first_failed{false};
    std::thread first([&] {  // blocks on the row, then fails for good
        try {
            pg::flush();
        } catch (const pg::error&) {
            first_failed = true;
        }
    });
    REQUIRE(lock_wait_seen());
    x += 1;  // arrives while the first write is in flight
    locker.exec("COMMIT");
    first.join();
    REQUIRE(first_failed);
    locker.exec("BEGIN");
    locker.exec("SELECT 1 FROM cache_cells WHERE id = 41 FOR UPDATE");
    std::thread second([] { pg::flush(); });  // writes the +1, blocked on the row
    REQUIRE(lock_wait_seen());
    std::int64_t seen = 0;
    std::thread reader([&] { seen = x.get(); });  // reloads while the +1 is in flight
    std::this_thread::sleep_for(100ms);
    locker.exec("COMMIT");
    second.join();
    reader.join();
    REQUIRE(db_n(41) == 31);
    REQUIRE(seen == 31);
    REQUIRE(x.get() == 31);
}

TEST_CASE("update leaves the value and the database unchanged when its function throws") {
    reset();
    errors e;
    configure(e);
    pg::cached<cells::label> label{43};
    REQUIRE(label.get() == std::nullopt);
    REQUIRE_THROWS_AS(label.update([](std::optional<std::string>& v) {
        v = "half done";
        throw std::runtime_error("boom");
    }),
                      std::runtime_error);
    REQUIRE(label.get() == std::nullopt);
    pg::flush();
    pg::conn c;
    REQUIRE(c.query_one<std::optional<std::string>>("SELECT label FROM cache_cells WHERE id = 43").value() == std::nullopt);
    REQUIRE(e.count() == 0);
}

// --- bulk write-back: many dirty cells are written by one statement per kind -------

namespace {

// Rows 61..10000 on top of reset()'s 60, all with n = 70.
void reset_many() {
    reset();
    pg::conn{}.exec("INSERT INTO cache_cells (id, code, n, ord) SELECT i, 'c' || i, 70, i FROM generate_series(61, 10000) i");
}

std::int64_t count_where(const std::string& where) {
    pg::conn c;
    return c.query_one<std::int64_t>("SELECT count(*) FROM cache_cells WHERE " + where).value();
}

}  // namespace

TEST_CASE("thousands of dirty cells, set and delta, simple and composite keys, reach exact values") {
    reset_many();
    errors e;
    configure(e);
    {
        pg::conn c;
        c.exec("DROP TABLE IF EXISTS cache_grid");
        c.exec("CREATE TABLE cache_grid (a int, b text, v bigint NOT NULL DEFAULT 5, note text, PRIMARY KEY (a, b))");
        c.exec("INSERT INTO cache_grid (a, b) SELECT i % 7, 'k' || i FROM generate_series(1, 3000) i");
    }
    for (int id = 1000; id < 3500; ++id) pg::cached<cells::n>{id} = id * 2;  // set
    for (int id = 3500; id < 6000; ++id) pg::cached<cells::n>{id} += id;     // delta
    const std::array<std::string, 6> notes{"plain", "with \"quote\"", "back\\slash", "{braces}, comma", "", "NULL"};
    for (int i = 1; i <= 3000; ++i) {
        const std::int32_t a = i % 7;
        const std::string b = "k" + std::to_string(i);
        if (i % 2) pg::cached<db::cache_grid::v>{a, b} = i;
        else pg::cached<db::cache_grid::v>{a, b} += i;
        if (i % 5 == 0) pg::cached<db::cache_grid::note>{a, b} = std::nullopt;
        else pg::cached<db::cache_grid::note>{a, b} = notes[static_cast<std::size_t>(i) % notes.size()];
    }
    pg::flush();
    REQUIRE(e.count() == 0);
    REQUIRE(count_where("id BETWEEN 1000 AND 3499 AND n = id * 2 AND writes = 1") == 2500);
    REQUIRE(count_where("id BETWEEN 3500 AND 5999 AND n = 70 + id AND writes = 1") == 2500);
    REQUIRE(count_where("(id < 1000 OR id >= 6000) AND (n <> 70 OR writes <> 0)") == 0);
    pg::conn c;
    const auto grid = [&](const std::string& where) {
        return c.query_one<std::int64_t>("SELECT count(*) FROM cache_grid WHERE " + where).value();
    };
    REQUIRE(grid("(CAST(substr(b, 2) AS int) % 2 = 1 AND v = CAST(substr(b, 2) AS int))"
                 " OR (CAST(substr(b, 2) AS int) % 2 = 0 AND v = 5 + CAST(substr(b, 2) AS int))") == 3000);
    REQUIRE(grid("CAST(substr(b, 2) AS int) % 5 = 0 AND note IS NULL") == 600);
    REQUIRE(grid("note = 'plain'") == 500 - 100);  // i % 6 == 0, minus the multiples of 5 among them
    REQUIRE(grid("note = 'with \"quote\"'") == 500 - 100);
    REQUIRE(grid("note = 'back\\slash'") == 500 - 100);
    REQUIRE(grid("note = '{braces}, comma'") == 500 - 100);
    REQUIRE(grid("note = ''") == 500 - 100);
    REQUIRE(grid("note = 'NULL'") == 500 - 100);
    pg::flush();  // nothing left to write
    REQUIRE(count_where("id BETWEEN 1000 AND 5999 AND writes <> 1") == 0);
}

TEST_CASE("one bad cell in a large group is dropped and reported; every other cell commits") {
    reset_many();
    errors e;
    configure(e);
    for (int id = 1000; id < 3500; ++id) pg::cached<cells::n>{id} = id;
    for (int id = 3500; id < 6000; ++id) pg::cached<cells::n>{id} += 1;
    pg::cached<cells::n>{2222} = -1;     // CHECK violation inside the set group
    pg::cached<cells::n>{4444} += -500;  // CHECK violation inside the delta group
    try {
        pg::flush();
        FAIL("flush did not throw");
    } catch (const pg::error& err) {
        REQUIRE(err.sqlstate == "23514");
    }
    REQUIRE(e.all() == std::vector<std::string>{"23514"});  // the other bad cell went to on_error
    REQUIRE(db_n(2222) == 70);
    REQUIRE(db_n(4444) == 70);
    REQUIRE(count_where("id BETWEEN 1000 AND 3499 AND id <> 2222 AND n = id AND writes = 1") == 2499);
    REQUIRE(count_where("id BETWEEN 3500 AND 5999 AND id <> 4444 AND n = 71 AND writes = 1") == 2499);
    pg::cached<cells::n> reloaded{2222};
    REQUIRE(reloaded.get() == 70);
    pg::flush();
    REQUIRE(e.count() == 1);
}

TEST_CASE("a deleted row in a large group is not_found for its cell only") {
    reset_many();
    errors e;
    configure(e);
    for (int id = 1000; id < 3500; ++id) pg::cached<cells::n>{id} = id;
    for (int id = 3500; id < 6000; ++id) pg::cached<cells::n>{id} += 3;
    pg::conn{}.exec("DELETE FROM cache_cells WHERE id IN (1500, 4500)");
    REQUIRE_THROWS_AS(pg::flush(), pg::not_found);
    REQUIRE(e.all() == std::vector<std::string>{"NF"});  // the second missing row
    REQUIRE(count_where("id BETWEEN 1000 AND 3499 AND n = id AND writes = 1") == 2499);
    REQUIRE(count_where("id BETWEEN 3500 AND 5999 AND n = 73 AND writes = 1") == 2499);
    pg::flush();
    REQUIRE(e.count() == 1);
}
