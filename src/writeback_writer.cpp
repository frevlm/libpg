// The writer thread of a writeback: batches, outcome handling, settlement,
// reads (loads and reconciles), receipt cleanup.
//
// Why a read is exact (CONTEXT.md 对齐): every database access of a
// writeback's counters runs on this one thread, between batches. When a read
// runs, no batch is in flight and none has an unknown outcome (a batch is
// replayed until its outcome is known before the thread does anything else),
// so this process's deltas in the database are exactly those settled as
// committed, and pending holds exactly those that are not. The read sees one
// committed snapshot V; setting value = V + pending under the slot's shard
// lock is then "committed + unsettled" for that snapshot. A submit racing
// with the read adds to value and pending under the same lock, before or
// after; either way the sum stays right.
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <exception>
#include <set>
#include <unordered_map>
#include <unordered_set>

#include "writeback.hpp"

namespace pg::detail {
namespace {

constexpr std::size_t max_postings = 5000;     // per batch
constexpr std::size_t max_statements = 200;    // distinct statements per batch (cache holds 256)
constexpr std::size_t read_chunk = 10'000;     // keys per read statement
constexpr auto cleanup_every = std::chrono::hours(1);
constexpr std::string_view missing_marker = "libpg writeback: a counter row does not exist";

// ---- encoding --------------------------------------------------------------

std::int64_t get_i64(const char* p) {
    std::uint64_t u = 0;
    for (int i = 0; i < 8; ++i) u = (u << 8) | static_cast<unsigned char>(p[i]);
    return static_cast<std::int64_t>(u);
}

param text_array(const std::vector<std::string>& texts) {
    std::vector<std::optional<std::string>> owned(texts.begin(), texts.end());
    std::vector<const std::optional<std::string>*> refs;
    for (const auto& t : owned) refs.push_back(&t);
    return array_param(text_oid, refs);
}

// Deltas as int8[] (integer counters) or numeric[] (decimal counters).
param delta_array(const counter& c, const std::vector<std::int64_t>& units) {
    std::vector<std::optional<std::string>> owned;
    for (std::int64_t u : units) {
        std::string b;
        if (c.scale < 0) codec<std::int64_t>::encode(u, b);
        else encode_scaled(u, c.scale, b);
        owned.emplace_back(std::move(b));
    }
    std::vector<const std::optional<std::string>*> refs;
    for (const auto& t : owned) refs.push_back(&t);
    return array_param(c.scale < 0 ? 20 : 1700, refs);
}

std::string qi(std::string_view name) { return quote_ident(name); }

// ---- statements ------------------------------------------------------------

// Statements of one operation with the storage they view. owner: the posting
// a statement belongs to (-1 none); c: the counter an UPDATE adds to.
struct stmt_list {
    std::vector<std::string> texts;
    std::vector<std::vector<param>> params;
    std::vector<int> owner;
    std::vector<const counter*> c;

    void add(std::string text, std::vector<param> ps = {}, int who = -1, const counter* cn = nullptr) {
        texts.push_back(std::move(text));
        params.push_back(std::move(ps));
        owner.push_back(who);
        c.push_back(cn);
    }
    std::vector<statement> view() const {
        std::vector<statement> v;
        for (std::size_t i = 0; i < texts.size(); ++i) v.push_back({texts[i], params[i], nullptr});
        return v;
    }
};


std::string cast_to(const column_types& t, std::string_view column) {
    const auto it = t.find(std::string(column));
    return it == t.end() ? std::string() : "::" + it->second.cast;
}

// ---- the session -----------------------------------------------------------

// Every statement of the writer joins unnest()ed keys to rows by their key.
// A custom plan knows the array length and, from a few hundred keys on,
// costs a hash join over a full scan of the table below index lookups, which
// is 10-30x slower on a cached table; the generic plan (unnest estimated at a
// few rows) looks the keys up by index. PostgreSQL's default plan_cache_mode
// settles on that generic plan only after five custom plans per statement.
constexpr std::string_view writer_settings = "-c plan_cache_mode=force_generic_plan";

session& ensure_session(wb_state& w) {
    if (w.s) return *w.s;
    pool_options o = w.p.options();
    if (!w.o.direct_dsn.empty()) {
        o.dsn = w.o.direct_dsn;
        o.pooler = false;
    }
    w.s.reset(open_session(o, writer_settings));
    std::lock_guard lk(w.pid_m);
    w.own_pids.push_back(PQbackendPID(w.s->c));
    return *w.s;
}

void drop_unusable(wb_state& w) {
    if (w.s && (w.s->broken || w.s->discard)) w.s.reset();
}

outcome run(wb_state& w, const stmt_list& l) {
    session& s = ensure_session(w);
    try {
        outcome o = execute(s, l.view());
        drop_unusable(w);
        return o;
    } catch (...) {
        drop_unusable(w);
        throw;
    }
}

// Runs statements that must succeed.
outcome run_ok(wb_state& w, const stmt_list& l) {
    outcome o = run(w, l);
    if (o.failure) throw std::move(*o.failure);
    return o;
}

// The column types of a table: name, cast text, and the base type's OID,
// category and kind.
const column_types& types_of(wb_state& w, std::string_view table) {
    const auto it = w.types.find(std::string(table));
    if (it != w.types.end()) return it->second;
    stmt_list l;
    l.add("SELECT a.attname::text, format_type(a.atttypid, a.atttypmod), b.oid::int8, "
          "b.typcategory::text, b.typtype::text "
          "FROM pg_attribute a "
          "JOIN pg_type t ON t.oid = a.atttypid "
          "JOIN pg_type b ON b.oid = CASE WHEN t.typtype = 'd' THEN t.typbasetype ELSE t.oid END "
          "WHERE a.attrelid = to_regclass($1) AND a.attnum > 0 AND NOT a.attisdropped",
          {to_param(qi(table))});
    const outcome o = run_ok(w, l);
    column_types t;
    const PGresult* r = o.results[0].get();
    for (int i = 0; i < PQntuples(r); ++i) {
        column_type c;
        c.cast = PQgetvalue(r, i, 1);
        c.base = static_cast<oid>(get_i64(PQgetvalue(r, i, 2)));
        c.category = *PQgetvalue(r, i, 3);
        c.kind = *PQgetvalue(r, i, 4);
        t[PQgetvalue(r, i, 0)] = std::move(c);
    }
    return w.types[std::string(table)] = std::move(t);
}

// ---- batch statements ------------------------------------------------------

struct row_group {
    std::vector<const wb_row*> rows;
};

// Rows insert as one statement when they write the same columns of the same
// table with the same element types.
bool same_shape(const wb_row& a, const wb_row& b) {
    if (a.table != b.table || a.columns.size() != b.columns.size()) return false;
    for (std::size_t i = 0; i < a.columns.size(); ++i)
        if (a.columns[i] != b.columns[i] || a.values[i].type != b.values[i].type) return false;
    return true;
}

std::size_t shape_hash(const wb_row& r) {
    std::size_t h = std::hash<std::string_view>{}(r.table) ^ r.columns.size();
    for (const wb_element& e : r.values) h = h * 31 + e.type;
    return h;
}

// INSERT INTO t (a, b) SELECT u.c1::ta, u.c2::tb FROM unnest($1, $2) AS u(c1, c2)
std::string insert_sql(wb_state& w, const wb_row& r) {
    const column_types& t = types_of(w, r.table);
    std::string cols, sel, args, names;
    for (std::size_t i = 0; i < r.columns.size(); ++i) {
        const std::string sep = i ? ", " : "";
        const std::string n = std::to_string(i + 1);
        cols += sep + qi(r.columns[i]);
        sel += sep + "u.c" + n + cast_to(t, r.columns[i]);
        args += sep + "$" + n;
        names += sep + "c" + n;
    }
    return "INSERT INTO " + qi(r.table) + " (" + cols + ") SELECT " + sel + " FROM unnest(" + args + ") AS u(" +
           names + ")";
}

std::vector<param> insert_params(const row_group& g) {
    std::vector<param> ps;
    const wb_row& first = *g.rows.front();
    for (std::size_t col = 0; col < first.columns.size(); ++col) {
        std::vector<const std::optional<std::string>*> elems;
        for (const wb_row* r : g.rows) elems.push_back(&r->values[col].bytes);
        ps.push_back(array_param(first.values[col].type, elems));
    }
    return ps;
}

// Whether an element's bytes are the column type's binary wire form.
bool element_copyable(const wb_element& e, const column_type& t) {
    return copy_accepts(e.type, copy_target{t.base, t.category, t.kind});
}

// Whether every column of a row can be sent by COPY. All rows of a group share
// columns and element types, so the first row decides the group.
bool copyable(const column_types& t, const wb_row& r) {
    for (std::size_t i = 0; i < r.columns.size(); ++i) {
        const auto it = t.find(std::string(r.columns[i]));
        if (it == t.end() || !element_copyable(r.values[i], it->second)) return false;
    }
    return true;
}

// One column's bytes with the jsonb version byte where the server wants it.
void put_column(std::string& out, const wb_element& e, const column_type& t) {
    if (!e.bytes) {
        copy_i32(-1, out);
        return;
    }
    const bool jsonb = e.type == text_oid && t.base == jsonb_oid;
    copy_i32(static_cast<std::int32_t>(e.bytes->size() + (jsonb ? 1 : 0)), out);
    if (jsonb) out += '\x01';
    out += *e.bytes;
}

// The binary COPY body of a group: preamble, one tuple per row, trailer.
std::string copy_body(const column_types& t, const row_group& g) {
    const wb_row& first = *g.rows.front();
    std::size_t size = 11 + 8 + 2;
    for (const wb_row* r : g.rows) {
        size += 2;
        for (const auto& v : r->values) size += 4 + (v.bytes ? v.bytes->size() + 1 : 0);
    }
    std::string b;
    b.reserve(size);
    put_copy_header(b);
    std::vector<const column_type*> types;
    for (std::string_view c : first.columns) types.push_back(&t.at(std::string(c)));
    const auto count = static_cast<std::int16_t>(types.size());
    for (const wb_row* r : g.rows) {
        copy_i16(count, b);
        for (std::size_t i = 0; i < types.size(); ++i) put_column(b, r->values[i], *types[i]);
    }
    put_copy_trailer(b);
    return b;
}

// Adds every key to every delta's slot; one UPDATE per counter. The final
// SELECT fails (22P02, a data error) unless every key matched a row, so a
// delta to a deleted row aborts the batch like any other data error.
std::string update_sql(wb_state& w, const counter& c) {
    const column_types& t = types_of(w, c.table);
    std::string args, names, where;
    const std::size_t n = c.key_columns.size();
    for (std::size_t i = 0; i < n; ++i) {
        const std::string k = "k" + std::to_string(i + 1);
        args += "$" + std::to_string(i + 1) + ", ";
        names += k + ", ";
        where += std::string(i ? " AND " : "") + qi(c.table) + "." + qi(c.key_columns[i]) + " = v." + k +
                 cast_to(t, c.key_columns[i]);
    }
    const std::string tc = qi(c.table) + "." + qi(c.column);
    return "WITH v AS (SELECT * FROM unnest(" + args + "$" + std::to_string(n + 1) + ") AS v(" + names +
           "d)), u AS (UPDATE " + qi(c.table) + " SET " + qi(c.column) + " = " + tc + " + v.d FROM v WHERE " +
           where + " RETURNING 1) SELECT CASE WHEN (SELECT count(*) FROM u) = (SELECT count(*) FROM v) THEN 0 ELSE ('" +
           std::string(missing_marker) + ": ' || (SELECT count(*) FROM v))::int END";
}

// Deltas summed per key, keys sorted, per counter in name order.
using key_sums = std::map<std::vector<std::string>, std::int64_t>;

std::vector<param> update_params(const counter& c, const key_sums& sums) {
    std::vector<std::vector<std::string>> cols(c.key_columns.size());
    std::vector<std::int64_t> units;
    for (const auto& [key, u] : sums) {
        for (std::size_t i = 0; i < cols.size(); ++i) cols[i].push_back(key[i]);
        units.push_back(u);
    }
    std::vector<param> ps;
    for (const auto& col : cols) ps.push_back(text_array(col));
    ps.push_back(delta_array(c, units));
    return ps;
}

// The group a row joins: an existing one of the same shape, else a new one.
std::size_t group_of(std::vector<row_group>& groups, std::unordered_map<std::size_t, std::vector<std::size_t>>& index,
                     const wb_row& r) {
    std::vector<std::size_t>& same_hash = index[shape_hash(r)];
    for (std::size_t g : same_hash)
        if (same_shape(*groups[g].rows.front(), r)) return g;
    same_hash.push_back(groups.size());
    groups.emplace_back();
    return groups.size() - 1;
}

// Rows grouped by shape: one group per statement.
std::vector<row_group> group_rows(std::span<posting_rec* const> ps) {
    std::vector<row_group> groups;
    std::unordered_map<std::size_t, std::vector<std::size_t>> index;
    for (const posting_rec* p : ps)
        for (const wb_row& r : p->rows) groups[group_of(groups, index, r)].rows.push_back(&r);
    return groups;
}

// Adds a group's rows as one unnest() insert.
void add_group(wb_state& w, stmt_list& l, const row_group& g, int owner) {
    l.add(insert_sql(w, *g.rows.front()), insert_params(g), owner);
}

void add_groups(wb_state& w, stmt_list& l, std::span<const row_group> gs, int owner) {
    for (const row_group& g : gs) add_group(w, l, g, owner);
}

void add_inserts(wb_state& w, stmt_list& l, std::span<posting_rec* const> ps, int owner) {
    add_groups(w, l, group_rows(ps), owner);
}

void add_updates(wb_state& w, stmt_list& l, std::span<posting_rec* const> ps, int owner) {
    std::map<std::string, std::pair<const counter*, key_sums>> by_counter;
    for (const posting_rec* p : ps) {
        for (const slot_delta& d : p->deltas) {
            auto& e = by_counter[d.s->c->name()];
            e.first = d.s->c;
            e.second[d.s->key] += d.units;
        }
    }
    for (const auto& [name, e] : by_counter) l.add(update_sql(w, *e.first), update_params(*e.first, e.second), owner, e.first);
}

void add_postings(wb_state& w, stmt_list& l, std::span<posting_rec* const> ps, int owner) {
    add_inserts(w, l, ps, owner);
    add_updates(w, l, ps, owner);
}

void add_receipt(stmt_list& l, const uuid& id) {
    l.add("INSERT INTO libpg.flush_receipts(id) VALUES ($1)", {to_param(id)});
}

// ---- settlement ------------------------------------------------------------

std::string describe_delta(const slot_delta& d) {
    std::string keys;
    for (const std::string& k : d.s->key) keys += (keys.empty() ? "" : ", ") + k;
    const std::string v = d.s->c->scale < 0 ? std::to_string(d.units) : format_scaled(d.units, d.s->c->scale);
    return d.s->c->name() + " (" + keys + ") += " + v;
}

std::string describe_row(const wb_row& r) {
    std::string s = "insert into " + std::string(r.table) + ":";
    for (std::size_t i = 0; i < r.columns.size(); ++i)
        s += (i ? ", " : " ") + std::string(r.columns[i]) + "=" + describe_element(r.values[i]);
    return s;
}

void settle_committed(wb_state& w, std::vector<posting_rec*> ps) {
    for (posting_rec* p : ps) settle_deltas(w, p->deltas, settle::committed);
    w.committed.fetch_add(ps.size());
    w.backlog.fetch_sub(ps.size());
}

void lose(wb_state& w, posting_rec& p, const std::string& reason, error_kind kind) {
    settle_deltas(w, p.deltas, settle::lost);
    lost_posting l{reason, kind, {}};
    for (const wb_row& r : p.rows) l.description.push_back(describe_row(r));
    for (const slot_delta& d : p.deltas) l.description.push_back(describe_delta(d));
    w.lost.fetch_add(1);
    w.backlog.fetch_sub(1);
    try {
        w.o.on_lost(l);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "pg::writeback: on_lost threw: %s\n", e.what());
    }
}

// The reason a posting failed: a missing counter row says which counter.
std::string reason_of(const error& e, const counter* c) {
    if (c && e.sqlstate == "22P02" && std::string_view(e.what()).find(missing_marker) != std::string_view::npos)
        return c->name() + ": the row of a delta does not exist";
    return e.what();
}

// ---- outcome of a batch ----------------------------------------------------

struct batch {
    uuid receipt;
    std::vector<posting_rec*> postings;
};

enum class step { committed, replay, isolate };

uuid new_receipt(wb_state& w) {
    uuid u;
    for (std::size_t i = 0; i < 16; i += 8) {
        const std::uint64_t r = w.rng();
        std::memcpy(u.bytes.data() + i, &r, 8);
    }
    u.bytes[6] = static_cast<std::uint8_t>((u.bytes[6] & 0x0f) | 0x40);
    u.bytes[8] = static_cast<std::uint8_t>((u.bytes[8] & 0x3f) | 0x80);
    return u;
}

step by_disposition(wb_state& w, const error& e) {
    w.last = e;
    return dispose(e.kind).writeback == disposition::writeback_action::replay ? step::replay : step::isolate;
}

// The test seam of inject_fault: the connection "lost" around the batch.
void fault_before(wb_state& w, wb_fault f) {
    if (f != wb_fault::lost_before_send) return;
    w.s.reset();
    throw error(error_kind::broken, "test fault: connection lost before the batch was sent");
}

void fault_after(wb_state& w, wb_fault f) {
    if (f != wb_fault::lost_after_commit) return;
    w.s.reset();
    throw error(error_kind::broken, "test fault: connection lost before the batch's result was read");
}

// Ends an explicit transaction left open; a connection that cannot is dropped.
void abandon_transaction(wb_state& w);

// Rows at least this many go by binary COPY rather than unnest() inserts.
constexpr std::size_t copy_min_rows = 64;

// A batch's row groups split by how they go: COPY or unnest() inserts.
struct copy_plan {
    std::vector<const row_group*> copy;
    std::vector<const row_group*> unnest;
};

copy_plan plan_groups(wb_state& w, std::span<const row_group> gs) {
    copy_plan p;
    for (const row_group& g : gs) {
        const wb_row& r = *g.rows.front();
        const bool by_copy = g.rows.size() >= copy_min_rows && copyable(types_of(w, r.table), r);
        (by_copy ? p.copy : p.unnest).push_back(&g);
    }
    return p;
}

// The test seam of inject_fault: the connection "lost" after the COPYs.
void fault_copy(wb_state& w, wb_fault f) {
    if (f != wb_fault::lost_after_copy) return;
    w.s.reset();
    throw error(error_kind::broken, "test fault: connection lost after the COPY");
}

// The batch in one round trip, as one implicit transaction (no BEGIN, one
// Sync): receipt, inserts, deltas. A 23505 on the receipt means an earlier
// send of this very batch committed.
step attempt_unnest(wb_state& w, const batch& b, wb_fault f) {
    stmt_list l;
    add_receipt(l, b.receipt);
    add_postings(w, l, b.postings, -1);
    outcome o = run(w, l);
    fault_after(w, f);
    if (!o.failure) return step::committed;
    if (o.failed == 0 && o.failure->sqlstate == "23505") return step::committed;
    return by_disposition(w, *o.failure);
}

// BEGIN and the receipt as one round trip. Returns the step to take instead of
// the COPYs when it failed; none means the transaction is open.
std::optional<step> copy_begin(wb_state& w, const batch& b) {
    stmt_list l;
    l.add("BEGIN");
    add_receipt(l, b.receipt);
    const outcome o = run(w, l);
    if (!o.failure) return std::nullopt;
    abandon_transaction(w);
    if (o.failed == 1 && o.failure->sqlstate == "23505") return step::committed;
    return by_disposition(w, *o.failure);
}

// The non-COPY groups, the deltas and COMMIT as one round trip.
step copy_finish(wb_state& w, const batch& b, const std::vector<const row_group*>& unnest) {
    stmt_list l;
    for (const row_group* g : unnest) add_group(w, l, *g, -1);
    add_updates(w, l, b.postings, -1);
    l.add("COMMIT");
    const outcome o = run(w, l);
    if (!o.failure) return step::committed;
    abandon_transaction(w);
    return by_disposition(w, *o.failure);
}

// The COPY path of a batch, in an explicit transaction.
step attempt_copy(wb_state& w, const batch& b, const copy_plan& plan, wb_fault f) {
    if (const auto early = copy_begin(w, b)) return *early;
    for (const row_group* g : plan.copy) {
        const wb_row& r = *g->rows.front();
        copy_in(*w.s, copy_statement(r.table, r.columns), copy_body(types_of(w, r.table), *g));
    }
    fault_copy(w, f);
    const step done = copy_finish(w, b, plan.unnest);
    fault_after(w, f);
    return done;
}

// Sends the batch; a failure leaves no transaction open.
step attempt(wb_state& w, const batch& b) {
    try {
        const auto f = static_cast<wb_fault>(w.fault.exchange(0));
        ensure_session(w);
        fault_before(w, f);
        const std::vector<row_group> groups = group_rows(b.postings);
        const copy_plan plan = plan_groups(w, groups);
        if (plan.copy.empty()) return attempt_unnest(w, b, f);
        return attempt_copy(w, b, plan, f);
    } catch (const error& e) {
        drop_unusable(w);
        abandon_transaction(w);
        return by_disposition(w, e);
    }
}

// ---- isolation: one savepoint per posting ----------------------------------

// Ends an explicit transaction left open; a connection that cannot is dropped.
void abandon_transaction(wb_state& w) {
    if (!w.s || idle(*w.s)) return;
    try {
        stmt_list l;
        l.add("ROLLBACK");
        run_ok(w, l);
    } catch (const error&) {
        w.s.reset();
    }
}

struct redo_state {
    batch& b;
    std::vector<bool> lost;
    std::vector<std::size_t> todo;  // postings not yet executed in the open transaction
    bool begun = false;             // the explicit transaction is open
};

// One round: [BEGIN …] each posting followed by RELEASE + SAVEPOINT, then COMMIT.
stmt_list redo_round(wb_state& w, redo_state& r) {
    stmt_list l;
    if (!r.begun) {
        l.add("BEGIN");
        l.add("SET CONSTRAINTS ALL IMMEDIATE");
        add_receipt(l, r.b.receipt);
        l.add("SAVEPOINT libpg_wb");
    }
    for (std::size_t i : r.todo) {
        const int who = static_cast<int>(i);
        add_postings(w, l, std::span(&r.b.postings[i], 1), who);
        l.add("RELEASE SAVEPOINT libpg_wb", {}, who);
        l.add("SAVEPOINT libpg_wb", {}, who);
    }
    l.add("COMMIT", {}, -2);
    return l;
}

// Postings whose trailing SAVEPOINT ran are in the transaction; the rest of
// the round (minus the failed one) runs again.
void after_failure(redo_state& r, const stmt_list& l, const outcome& o, std::size_t failed) {
    std::set<std::size_t> done;
    for (std::size_t k = 0; k < l.texts.size(); ++k)
        if (l.owner[k] >= 0 && o.results[k] && l.texts[k].starts_with("SAVEPOINT")) done.insert(static_cast<std::size_t>(l.owner[k]));
    std::erase_if(r.todo, [&](std::size_t i) { return i == failed || done.contains(i); });
}

void lose_rest(wb_state& w, redo_state& r, const error& e) {
    for (std::size_t i = 0; i < r.b.postings.size(); ++i) {
        if (r.lost[i]) continue;
        r.lost[i] = true;
        lose(w, *r.b.postings[i], e.what(), e.kind);
    }
}

std::vector<posting_rec*> kept(const redo_state& r) {
    std::vector<posting_rec*> out;
    for (std::size_t i = 0; i < r.b.postings.size(); ++i)
        if (!r.lost[i]) out.push_back(r.b.postings[i]);
    return out;
}

// A posting failed: it is rolled back to its savepoint and lost. When the
// transaction never began (the failure came before BEGIN executed), the
// round restarts from scratch.
step drop_posting(wb_state& w, redo_state& r, const stmt_list& l, const outcome& o) {
    const error& e = *o.failure;
    const auto i = static_cast<std::size_t>(l.owner[o.failed]);
    r.lost[i] = true;
    lose(w, *r.b.postings[i], reason_of(e, l.c[o.failed]), e.kind);
    if (idle(*w.s)) {
        r.begun = false;
        r.todo.clear();
        for (std::size_t k = 0; k < r.b.postings.size(); ++k)
            if (!r.lost[k]) r.todo.push_back(k);
        return step::isolate;
    }
    after_failure(r, l, o, i);
    stmt_list back;
    back.add("ROLLBACK TO SAVEPOINT libpg_wb");
    run_ok(w, back);
    return step::isolate;
}

// One round of the redo; isolate means "another round".
step redo_once(wb_state& w, redo_state& r) {
    const stmt_list l = redo_round(w, r);
    const outcome o = run(w, l);
    if (!o.failure) return step::committed;
    if (by_disposition(w, *o.failure) == step::replay) return step::replay;
    if (l.owner[o.failed] < 0) {
        lose_rest(w, r, *o.failure);
        return step::committed;
    }
    if (!w.s) return step::replay;  // the connection went with the error: the round's outcome is unknown
    r.begun = !idle(*w.s);
    return drop_posting(w, r, l, o);
}

// Redoes a batch that hit a data error in an explicit transaction with a new
// receipt: failing postings are lost, the rest commit. On a replay
// disposition the batch keeps the new receipt and the postings not lost, for
// the caller to replay. Returns true when the batch is settled.
bool redo(wb_state& w, batch& b) {
    w.types.clear();
    b.receipt = new_receipt(w);
    redo_state r{b, std::vector<bool>(b.postings.size()), {}};
    for (std::size_t i = 0; i < b.postings.size(); ++i) r.todo.push_back(i);
    step s = step::isolate;
    try {
        while (s == step::isolate) s = redo_once(w, r);
    } catch (const error& e) {
        s = by_disposition(w, e);
        if (s == step::isolate) lose_rest(w, r, e);
    }
    abandon_transaction(w);
    b.postings = kept(r);
    if (s == step::replay) return b.postings.empty();
    settle_committed(w, b.postings);
    return true;
}

// ---- reads -----------------------------------------------------------------

// SELECT v.n, t.c FROM unnest($1..) WITH ORDINALITY AS v(k1.., n) JOIN t ON …
std::string read_sql(wb_state& w, const counter& c) {
    const column_types& t = types_of(w, c.table);
    std::string args, names, on;
    for (std::size_t i = 0; i < c.key_columns.size(); ++i) {
        const std::string k = "k" + std::to_string(i + 1);
        args += std::string(i ? ", " : "") + "$" + std::to_string(i + 1);
        names += k + ", ";
        on += std::string(i ? " AND " : "") + "t." + qi(c.key_columns[i]) + " = v." + k + cast_to(t, c.key_columns[i]);
    }
    const std::string val = "t." + qi(c.column) + (c.scale < 0 ? "::int8" : "::numeric");
    return "SELECT v.n, " + val + " FROM unnest(" + args + ") WITH ORDINALITY AS v(" + names + "n) JOIN " +
           qi(c.table) + " t ON " + on;
}

std::int64_t value_units(const counter& c, const PGresult* r, int row) {
    const char* v = PQgetvalue(r, row, 1);
    if (c.scale < 0) return get_i64(v);
    return decode_scaled(std::string_view(v, static_cast<std::size_t>(PQgetlength(r, row, 1))), c.scale);
}

// The committed values of the keys (none: no such row).
std::vector<std::optional<std::int64_t>> read_values(wb_state& w, const counter& c,
                                                     const std::vector<std::vector<std::string>>& keys) {
    std::vector<std::optional<std::int64_t>> out(keys.size());
    for (std::size_t from = 0; from < keys.size(); from += read_chunk) {
        const std::size_t to = std::min(keys.size(), from + read_chunk);
        std::vector<std::vector<std::string>> cols(c.key_columns.size());
        for (std::size_t k = from; k < to; ++k)
            for (std::size_t i = 0; i < cols.size(); ++i) cols[i].push_back(keys[k][i]);
        std::vector<param> ps;
        for (const auto& col : cols) ps.push_back(text_array(col));
        stmt_list l;
        l.add(read_sql(w, c), std::move(ps));
        const outcome o = run_ok(w, l);
        const PGresult* r = o.results[0].get();
        for (int row = 0; row < PQntuples(r); ++row)
            out[from + static_cast<std::size_t>(get_i64(PQgetvalue(r, row, 0)) - 1)] = value_units(c, r, row);
    }
    return out;
}

// value = committed + pending (see the comment at the top of the file).
void reconcile_slots(wb_state& w, const counter& c, const std::vector<slot*>& ss) {
    std::vector<std::vector<std::string>> keys;
    for (const slot* s : ss) keys.push_back(s->key);
    const auto vals = read_values(w, c, keys);
    for (std::size_t i = 0; i < ss.size(); ++i) {
        const auto lk = w.slots.lock(ss[i]->shard);
        ss[i]->loaded = vals[i].has_value();
        if (vals[i]) ss[i]->value = *vals[i] + ss[i]->pending;
    }
}

std::vector<counter*> counters_of(wb_state& w, bool watched_only) {
    std::vector<counter*> out;
    std::lock_guard lk(w.reg_m);
    for (auto& [k, c] : w.counters)
        if (!watched_only || c->watched.load()) out.push_back(c.get());
    return out;
}

void reconcile_all(wb_state& w, bool watched_only) {
    for (counter* c : counters_of(w, watched_only)) reconcile_slots(w, *c, w.slots.loaded_of(c));
    w.full_reconciles.fetch_add(1);
}

void reconcile_notified(wb_state& w, const notified_keys& n) {
    if (n.all) {
        const std::vector<slot*> ss = w.slots.loaded_of(n.c);
        reconcile_slots(w, *n.c, ss);
        w.reconciled.fetch_add(ss.size());
        return;
    }
    std::vector<slot*> ss;
    for (const auto& key : n.keys) {
        const std::string mk = slot_key(n.c->table, n.c->column, key);
        const std::size_t shard = slot_table::shard_of(mk);
        const auto lk = w.slots.lock(shard);
        slot* s = w.slots.find(shard, mk);
        if (s && s->loaded) ss.push_back(s);
    }
    if (ss.empty()) return;
    reconcile_slots(w, *n.c, ss);
    w.reconciled.fetch_add(ss.size());
}

void load(wb_state& w, read_req& q) {
    const auto vals = read_values(w, *q.c, {q.key});
    if (!vals[0]) throw not_found("no row in " + q.c->table + " for this key of " + q.c->name());
    const std::string mk = slot_key(q.c->table, q.c->column, q.key);
    const std::size_t shard = slot_table::shard_of(mk);
    const auto lk = w.slots.lock(shard);
    slot& s = w.slots.emplace(shard, mk, q.c, q.key);
    s.value = *vals[0] + s.pending;
    s.loaded = true;
    q.result.set_value(s.value);
}

void serve(wb_state& w, read_req& q) {
    try {
        if (q.c) return load(w, q);
        reconcile_all(w, false);
        q.result.set_value(0);
    } catch (const error&) {
        q.result.set_exception(std::current_exception());
    }
}

void fail_reads(std::vector<std::shared_ptr<read_req>> qs, const error& e) {
    for (auto& q : qs) q->result.set_exception(std::make_exception_ptr(e));
}

void serve_reads(wb_state& w) {
    std::vector<std::shared_ptr<read_req>> qs;
    std::vector<notified_keys> ns;
    bool full = false;
    {
        std::lock_guard lk(w.m);
        qs.swap(w.reads);
        ns.swap(w.notified);
        full = std::exchange(w.full_due, false);
    }
    for (auto& q : qs) serve(w, *q);
    try {
        for (const notified_keys& n : ns) reconcile_notified(w, n);
        if (full) reconcile_all(w, true);
    } catch (const error&) {
        std::lock_guard lk(w.m);
        w.full_due = true;  // retried next cycle; a full reconcile covers the notified keys
    }
}

// ---- sending ---------------------------------------------------------------

bool past_shutdown(wb_state& w) {
    if (!w.stopping.load()) return false;
    std::lock_guard lk(w.m);
    return wb_clock::now() >= w.stop_deadline;
}

// Waits before attempt `attempt + 1` (never past the shutdown deadline);
// reads that arrive meanwhile fail with the error that caused the wait.
void backoff(wb_state& w, int attempt) {
    const int base = 20 << std::min(attempt, 6);  // ms: 20 … 1280
    const auto delay = std::chrono::milliseconds(base / 2 + static_cast<int>(w.rng() % static_cast<unsigned>(base / 2)));
    auto until = wb_clock::now() + delay;
    std::unique_lock lk(w.m);
    if (w.stop) until = std::min(until, w.stop_deadline);
    while (w.cv.wait_until(lk, until, [&] { return !w.reads.empty(); })) {
        auto qs = std::exchange(w.reads, {});
        lk.unlock();
        fail_reads(std::move(qs), *w.last);
        lk.lock();
    }
}

void give_up(wb_state& w, batch& b) {
    const error e = w.last.value_or(error(error_kind::other, "shutdown"));
    for (posting_rec* p : b.postings)
        lose(w, *p, std::string("not written within shutdown_timeout: ") + e.what(), e.kind);
}

void send_until_settled(wb_state& w, batch& b) {
    for (int n = 0;; ++n) {
        if (past_shutdown(w)) return give_up(w, b);
        const step s = attempt(w, b);
        if (s == step::committed) return settle_committed(w, b.postings);
        if (s == step::isolate && redo(w, b)) return;
        w.replays.fetch_add(1);
        backoff(w, n);
    }
}

std::vector<posting_rec> drain(wb_state& w) {
    std::vector<posting_rec> all;
    for (auto& q : w.queues) {
        // The queue keeps room for as many postings as it had last time.
        std::vector<posting_rec> v;
        v.reserve(q.last);
        {
            std::lock_guard lk(q.m);
            v.swap(q.v);
        }
        q.last = v.size();
        for (auto& p : v) all.push_back(std::move(p));
    }
    return all;
}

// Names the statements of one drain: a row by the first row of its shape, a
// delta by its counter.
struct statement_ids {
    std::unordered_map<std::size_t, std::vector<const wb_row*>> shapes;

    const void* of(const wb_row& r) {
        std::vector<const wb_row*>& same = shapes[shape_hash(r)];
        for (const wb_row* s : same)
            if (same_shape(*s, r)) return s;
        same.push_back(&r);
        return &r;
    }
};

// The statements a posting adds to a batch: its row groups and its counters.
std::vector<const void*> statements_of(statement_ids& ids, const posting_rec& p) {
    std::vector<const void*> out;
    for (const wb_row& r : p.rows) out.push_back(ids.of(r));
    for (const slot_delta& d : p.deltas) out.push_back(d.s->c);
    return out;
}

// How many statements of `mine` are not yet in `seen`.
std::size_t fresh_of(const std::vector<const void*>& mine, const std::unordered_set<const void*>& seen) {
    std::unordered_set<const void*> out;
    for (const void* s : mine)
        if (!seen.contains(s)) out.insert(s);
    return out.size();
}

// Splits the postings into batches small enough for one operation.
std::vector<batch> batches_of(wb_state& w, std::vector<posting_rec>& all) {
    std::vector<batch> out;
    statement_ids ids;
    std::unordered_set<const void*> seen;
    for (posting_rec& p : all) {
        const std::vector<const void*> mine = statements_of(ids, p);
        const std::size_t fresh = fresh_of(mine, seen);
        if (out.empty() || out.back().postings.size() >= max_postings || seen.size() + fresh > max_statements) {
            out.push_back({new_receipt(w), {}});
            seen.clear();
        }
        seen.insert(mine.begin(), mine.end());
        out.back().postings.push_back(&p);
    }
    return out;
}

bool send_all(wb_state& w) {
    std::vector<posting_rec> all = drain(w);
    if (all.empty()) return false;
    for (batch& b : batches_of(w, all)) send_until_settled(w, b);
    return true;
}

// ---- the loop --------------------------------------------------------------

void clean_receipts(wb_state& w) {
    const auto now = wb_clock::now();
    if (now < w.next_cleanup) return;
    try {
        stmt_list l;
        l.add("DELETE FROM libpg.flush_receipts WHERE created_at < now() - interval '1 day'");
        run_ok(w, l);
        w.next_cleanup = now + cleanup_every;
    } catch (const error&) {
        w.next_cleanup = now + std::chrono::minutes(1);
    }
}

void maintain(wb_state& w) {
    clean_receipts(w);
    if (wb_clock::now() < w.next_full) return;
    w.next_full = wb_clock::now() + w.o.full_reconcile;
    if (counters_of(w, true).empty()) return;
    std::lock_guard lk(w.m);
    w.full_due = true;
}

void cycle(wb_state& w, bool send) {
    maintain(w);
    serve_reads(w);
    if (!send) return;
    std::uint64_t ticket;
    {
        std::lock_guard lk(w.m);
        ticket = w.flush_req;
    }
    send_all(w);
    {
        std::lock_guard lk(w.m);
        w.flush_done = std::max(w.flush_done, ticket);
    }
    w.done_cv.notify_all();
}

void shutdown(wb_state& w) {
    for (;;) {
        std::vector<std::shared_ptr<read_req>> qs;
        {
            std::lock_guard lk(w.m);
            qs.swap(w.reads);
        }
        fail_reads(std::move(qs), error(error_kind::other, "writeback is shutting down"));
        if (!send_all(w)) break;
    }
    {
        std::lock_guard lk(w.m);
        w.flush_done = w.flush_req;
    }
    w.done_cv.notify_all();
    w.s.reset();
}

wb_clock::time_point next_wake(const wb_state& w, wb_clock::time_point tick) {
    return std::min({tick, w.next_cleanup, w.next_full});
}

}  // namespace

void writer_loop(wb_state& w) {
    auto tick = wb_clock::now() + w.o.interval;
    for (;;) {
        bool send = false;
        {
            std::unique_lock lk(w.m);
            w.cv.wait_until(lk, next_wake(w, tick), [&] { return w.wake || w.stop; });
            w.wake = false;
            if (w.stop) break;
            send = wb_clock::now() >= tick || w.flush_req > w.flush_done;
        }
        if (send) tick = wb_clock::now() + w.o.interval;
        try {
            cycle(w, send);
        } catch (const std::exception& e) {
            std::fprintf(stderr, "pg::writeback: writer cycle failed: %s\n", e.what());
        }
    }
    shutdown(w);
}

}  // namespace pg::detail
