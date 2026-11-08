#include "prefetch.hpp"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <exception>
#include <new>
#include <string_view>
#include <utility>

namespace pg {
namespace detail {
namespace {

constexpr std::size_t max_trace = 32;          // reads of a scope the pool learns from
constexpr int max_rows_searched = 1000;        // larger results are no parameter source
constexpr double decay = 0.97;                 // per scope of the root
constexpr double min_ratio = 0.95;             // share of scopes a follower must appear in
constexpr std::uint64_t min_scopes = 8;        // scopes of a root before it is planned
constexpr std::size_t max_roots = 4096;
constexpr std::size_t max_followers = 64;      // per root
constexpr int max_keys = 1000;                 // keys a partitioned statement slices
constexpr double max_root_ns = 50e6;           // a slower root is not planned
constexpr double max_root_rows = 1000;         // a larger root is not planned
constexpr std::uint64_t barren_after = 64;     // scopes before a root without followers is sampled
constexpr std::uint64_t barren_sample = 64;    // one scope in this many of a barren root is observed
constexpr double faded = 0.05;                 // a follower below this share of scopes is dropped
constexpr std::uint64_t pause_scopes = 32;     // scopes a follower is not planned after an error
constexpr std::size_t max_kept_bytes = 1 << 20;  // larger results are no parameter source

void compile(prefetch_plan& pl, const sql& root);

bool ident_char(char c) { return ascii_letter(c) || c == '_' || (c >= '0' && c <= '9'); }

// Whether text at i is the word w (lower case) followed by a non-identifier character.
bool word_at(std::string_view t, std::size_t i, std::string_view w) {
    return folded_at(t, i, w) && (i + w.size() == t.size() || !ident_char(t[i + w.size()]));
}

// The first character at or after i that is not a blank or in a comment.
std::size_t skip_blank(std::string_view t, std::size_t i) { return i + first_token(t.substr(i)); }

// Whether the word FOR at k starts a locking clause: FOR UPDATE, FOR NO KEY
// UPDATE, FOR SHARE, FOR KEY SHARE.
bool locks_at(std::string_view t, std::size_t k) {
    if (k == 0 || k + 3 >= t.size() || !word_at(t, k, "for") || ident_char(t[k - 1])) return false;
    const std::size_t i = skip_blank(t, k + 3);
    return word_at(t, i, "update") || word_at(t, i, "share") || word_at(t, i, "no") || word_at(t, i, "key");
}

// Whether the statement locks rows; looks only where an F is.
bool locks_rows(std::string_view t) {
    const char* const begin = t.data();
    const char* const end = begin + t.size();
    for (const char f : {'f', 'F'})
        for (const char* p = begin; (p = static_cast<const char*>(std::memchr(p, f, static_cast<std::size_t>(end - p))));
             ++p)
            if (locks_at(t, static_cast<std::size_t>(p - begin))) return true;
    return false;
}

// Whether the statement's first word is SELECT.
bool selects(std::string_view t) {
    const std::size_t i = first_token(t);
    if (i + 6 > t.size()) return false;
    const char* p = t.data() + i;
    const bool word = (p[0] | 0x20) == 's' && (p[1] | 0x20) == 'e' && (p[2] | 0x20) == 'l' && (p[3] | 0x20) == 'e' &&
                      (p[4] | 0x20) == 'c' && (p[5] | 0x20) == 't';
    return word && (i + 6 == t.size() || !ident_char(p[6]));
}

// A read the scope may learn from and answer: a SELECT that locks nothing.
bool candidate(std::string_view text) { return selects(text) && !locks_rows(text); }

// A fast hash of a root text for the table's filters.
std::uint64_t text_hash(std::string_view t) {
    constexpr std::uint64_t mul = 0x9E3779B97F4A7C15ull;
    std::uint64_t h = t.size() * mul;
    std::size_t i = 0;
    for (; i + 8 <= t.size(); i += 8) {
        std::uint64_t w;
        std::memcpy(&w, t.data() + i, 8);
        h = (h ^ w) * mul;
        h ^= h >> 29;
    }
    std::uint64_t w = 0;
    std::memcpy(&w, t.data() + i, t.size() - i);
    h = (h ^ w) * mul;
    return h ^ (h >> 32);
}

void put_param(std::string& k, const param& p) {
    char head[6];
    std::memcpy(head, &p.type, 4);
    head[4] = p.binary ? 'b' : 't';
    head[5] = p.bytes ? 'v' : 'n';
    k.append(head, sizeof head);
    if (!p.bytes) return;
    const std::uint64_t n = p.bytes->size();
    k.append(reinterpret_cast<const char*>(&n), sizeof n);
    k += *p.bytes;
}

std::uint64_t mix(std::uint64_t h, std::uint64_t v) {
    constexpr std::uint64_t mul = 0x9E3779B97F4A7C15ull;
    h = (h ^ v) * mul;
    return h ^ (h >> 29);
}

// A hash of a read: its text and its parameters' types, formats and bytes.
std::uint64_t key_hash(std::string_view text, std::span<const param> ps) {
    std::uint64_t h = text_hash(text);
    for (const param& p : ps) {
        h = mix(h, p.type | std::uint64_t{p.binary} << 32 | std::uint64_t{p.bytes.has_value()} << 33);
        if (p.bytes) h = mix(h, text_hash(*p.bytes));
    }
    return h;
}

read_key make_key(std::string_view text, std::vector<param> ps) {
    const std::uint64_t h = key_hash(text, ps);
    return {h, text, std::move(ps)};
}

std::shared_ptr<held> make_held(shared_result r, std::span<const type_desc> cols, std::shared_ptr<const void> keep,
                                const planned* read, param_types ptypes, window w = {}) {
    return std::make_shared<held>(held{std::move(r), w, std::move(keep), cols, std::move(ptypes), read});
}

reply view_of(const std::shared_ptr<held>& h) {
    return {result(h->r.get(), result_deleter{false}), h->cols, h, h->w};
}

// Whether the part of a result that a window shows may be a parameter source:
// few rows and little memory. A result within the limit is; a larger one only
// when the window's own `cols` columns are.
bool searchable(const PGresult* r, const window& w, std::size_t cols) {
    const int rows = rows_in(r, w);
    if (rows > max_rows_searched) return false;
    if (PQresultMemorySize(r) <= max_kept_bytes) return true;
    if (w.rows < 0) return false;
    std::size_t bytes = 0;
    for (int row = w.row; row < w.row + rows; ++row)
        for (std::size_t c = 0; c < cols; ++c) bytes += static_cast<std::size_t>(PQgetlength(r, row, w.col + static_cast<int>(c)));
    return bytes <= max_kept_bytes;
}

// Adds a result that answers `copies` reads of the key.
void push_cached(prefetch_scope& sc, read_key key, std::shared_ptr<held> h, int copies = 1) {
    if (copies <= 0) return;
    sc.cache.push_back({std::move(key), std::move(h), copies});
    sc.cached += static_cast<std::size_t>(copies);
}

// Entries a cache may hold before it is searched through its index.
constexpr std::size_t scan_limit = 64;

bool answers(const cache_entry& e, std::uint64_t hash, std::string_view text, std::span<const param> ps) {
    return e.h && e.key.hash == hash && e.key.text == text && std::ranges::equal(e.key.params, ps);
}

// The oldest entry that still answers a read, or nullptr. A small cache is
// scanned from `head`; a larger one is searched by hash in an index built when
// entries were added since it was.
cache_entry* find_cached(prefetch_scope& sc, std::string_view text, std::span<const param> ps) {
    const std::uint64_t hash = key_hash(text, ps);
    if (sc.cache.size() <= scan_limit) {
        for (std::size_t i = sc.head; i < sc.cache.size(); ++i)
            if (answers(sc.cache[i], hash, text, ps)) return &sc.cache[i];
        return nullptr;
    }
    if (sc.order.size() != sc.cache.size()) {
        sc.order.clear();
        for (std::size_t i = 0; i < sc.cache.size(); ++i)
            sc.order.emplace_back(sc.cache[i].key.hash, static_cast<std::uint32_t>(i));
        std::sort(sc.order.begin(), sc.order.end());
    }
    for (auto it = std::lower_bound(sc.order.begin(), sc.order.end(), std::pair<std::uint64_t, std::uint32_t>{hash, 0});
         it != sc.order.end() && it->first == hash; ++it)
        if (answers(sc.cache[it->second], hash, text, ps)) return &sc.cache[it->second];
    return nullptr;
}

// Takes the result an entry answers a read with.
std::shared_ptr<held> serve_cached(prefetch_scope& sc, cache_entry& e) {
    --sc.cached;
    std::shared_ptr<held> h;
    if (--e.left > 0) h = e.h;
    else h = std::move(e.h);
    while (sc.head < sc.cache.size() && !sc.cache[sc.head].h) ++sc.head;
    if (sc.head == sc.cache.size()) sc.forget();
    return h;
}

// Drops what the scope prefetched and has not served.
void drop_cache(prefetch_scope& sc) {
    sc.unused += sc.cached;
    sc.forget();
    sc.cached = 0;
}

// Records a read the server ran and returns the reply the program reads. A
// result small enough to be a parameter source is kept by the trace; the
// reply views it (the program reads it before its next call on the conn).
reply record(prefetch_scope& sc, rendered& r, const sql* q, reply rp, param_types ptypes) {
    trace_entry& e = sc.trace.emplace_back();
    e.text = std::move(r.text);
    if (q) e.q = *q;
    e.params.insert_at_end(r.params.begin(), r.params.end());
    e.ptypes = std::move(ptypes);
    e.rows = rows_in(rp);
    e.w = rp.w;
    e.keep = std::move(rp.keep);  // the reply's columns stay valid: the trace outlives the reply
    e.cols = rp.columns;
    if (searchable(rp.r.get(), rp.w, rp.columns.size())) {
        e.own = std::move(rp.r);
        e.r = e.own.get();
        rp.r = result(const_cast<PGresult*>(e.r), result_deleter{false});
    }
    return rp;
}

// Records a read answered by a prefetched result.
void record_held(prefetch_scope& sc, rendered& r, const sql* q, const std::shared_ptr<held>& h) {
    trace_entry& e = sc.trace.emplace_back();
    e.text = std::move(r.text);
    if (q) e.q = *q;
    e.params.insert_at_end(r.params.begin(), r.params.end());
    e.ptypes = h->ptypes;
    e.rows = rows_in(h->r.get(), h->w);
    e.w = h->w;
    e.from = h;
    e.read = h->read;
    if (searchable(h->r.get(), h->w, h->cols.size())) e.r = h->r.get();
    e.keep = h;
    e.cols = h->cols;
}

// --- provenance ------------------------------------------------------------------

// A column can stand for a parameter when its type is the one the server
// resolved for the parameter, and the parameter's bytes are that column's
// binary form: a binary parameter of that type, or a text parameter of a string type.
bool type_fits(const param& p, oid resolved, const type_desc& t) {
    if (resolved == 0 || resolved != t.id) return false;
    return p.binary ? p.type == resolved : t.category == 'S';
}

// The column of the entry's result holding p's bytes in some row; -1 when none.
int column_of(const param& p, oid resolved, const trace_entry& e) {
    if (!p.bytes || !e.r) return -1;
    const PGresult* r = e.r;
    const int rows = e.rows;
    const std::string& b = *p.bytes;
    for (std::size_t c = 0; c < e.cols.size(); ++c) {
        if (!type_fits(p, resolved, e.cols[c])) continue;
        const int col = static_cast<int>(c);
        for (int row = e.w.row; row < e.w.row + rows; ++row) {
            if (PQgetisnull(r, row, e.w.col + col)) continue;
            if (static_cast<std::size_t>(PQgetlength(r, row, e.w.col + col)) != b.size()) continue;
            if (std::memcmp(PQgetvalue(r, row, e.w.col + col), b.data(), b.size()) == 0) return col;
        }
    }
    return -1;
}

// One follower signature seen in a scope.
struct observed {
    std::string key;
    std::string parent_key;
    int entry = 0;   // the first trace entry with this signature
    int parent = 0;  // the trace entry its column sources come from; 0 the root
    std::vector<source> src;
    int execs = 0;
    std::int64_t rows = 0;
};

void put_source(std::string& k, const source& s, const param& p) {
    switch (s.kind) {
        case source::root_param: k += 'r'; k += std::to_string(s.index); break;
        case source::column: k += 'c'; k += std::to_string(s.index); break;
        case source::literal: k += 'l'; put_param(k, p); break;
    }
    k += '\x1f';
}

// The follower signatures of a trace. A parameter's source is a root parameter
// with the same type and bytes, else a column of the root or of an earlier
// as-is follower holding its bytes, else a literal. A follower's column sources
// all come from one parent.
std::vector<observed> observe(const std::vector<trace_entry>& tr) {
    std::vector<observed> out;
    std::vector<std::string> keys(tr.size());
    std::vector<char> as_is(tr.size(), 0);
    const small_vec<param, 4>& root = tr[0].params;
    const auto count = [&](std::string key, std::size_t j) {
        auto it = std::find_if(out.begin(), out.end(), [&](const observed& o) { return o.key == key; });
        if (it == out.end()) {
            out.emplace_back();
            it = out.end() - 1;
            it->key = std::move(key);
            it->entry = static_cast<int>(j);
        }
        ++it->execs;
        it->rows += tr[j].rows;
        return it;
    };
    for (std::size_t j = 1; j < tr.size(); ++j) {
        const trace_entry& e = tr[j];
        if (e.read) {
            // Answered by the plan: its signature is the planned read's.
            keys[j] = e.read->key;
            as_is[j] = !std::any_of(e.read->src.begin(), e.read->src.end(),
                                    [](const source& s) { return s.kind == source::column; });
            count(keys[j], j);
            continue;
        }
        const bool resolved = e.ptypes && e.ptypes->size() == e.params.size();
        std::vector<source> src(e.params.size());
        int parent = 0;
        bool keyed = false;
        for (std::size_t m = 0; m < e.params.size(); ++m) {
            const param& p = e.params[m];
            const auto rp = std::find(root.begin(), root.end(), p);
            if (rp != root.end()) {
                src[m] = {source::root_param, static_cast<int>(rp - root.begin())};
                continue;
            }
            for (std::size_t i = 0; i < j && resolved; ++i) {
                if ((keyed && static_cast<int>(i) != parent) || (i > 0 && !as_is[i])) continue;
                const int col = column_of(p, (*e.ptypes)[m], tr[i]);
                if (col < 0) continue;
                src[m] = {source::column, col};
                parent = static_cast<int>(i);
                keyed = true;
                break;
            }
        }
        std::string key = e.text;
        key += '\x1e';
        if (keyed && parent > 0) key += keys[static_cast<std::size_t>(parent)];
        key += '\x1e';
        for (std::size_t m = 0; m < src.size(); ++m) put_source(key, src[m], e.params[m]);
        keys[j] = key;
        as_is[j] = !keyed;
        const auto it = count(std::move(key), j);
        if (it->execs > 1) continue;
        if (keyed && parent > 0) it->parent_key = keys[static_cast<std::size_t>(parent)];
        it->parent = keyed ? parent : 0;
        it->src = std::move(src);
    }
    return out;
}

// --- learning --------------------------------------------------------------------

bool has_column_source(const planned& p) {
    return std::any_of(p.src.begin(), p.src.end(), [](const source& s) { return s.kind == source::column; });
}

std::vector<col_info> info_of(std::span<const type_desc> cols) {
    std::vector<col_info> out;
    out.reserve(cols.size());
    for (const type_desc& t : cols) out.push_back({t.id, std::string(t.name), t.category, t.kind});
    return out;
}

bool same_types(const std::vector<col_info>& a, std::span<const type_desc> b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i)
        if (a[i].id != b[i].id) return false;
    return true;
}

// The plan of the planned followers. The followers keyed on a parent are
// joined to it in one unified statement that also serves the parent, at most
// one of them with several rows per key; as-is reads join the root's when the
// root has at most one row; the other followers of several rows per key get a
// partitioned statement each. The root always travels first in the batch.
std::shared_ptr<prefetch_plan> build(const root_stat& rs, const std::vector<char>& on) {
    auto pl = std::make_shared<prefetch_plan>();
    pl->root_types = rs.types;
    const int root_cols = static_cast<int>(rs.types.size());
    std::vector<double> rows;
    // As-is reads first, so a keyed one finds its parent's index.
    for (int pass = 0; pass < 2; ++pass)
        for (std::size_t i = 0; i < on.size(); ++i) {
            if (!on[i] || has_column_source(rs.followers[i].p) != (pass == 1)) continue;
            const follower_stat& f = rs.followers[i];
            planned p = f.p;
            p.parent = -1;
            for (std::size_t r = 0; r < pl->reads.size() && !f.parent_key.empty(); ++r)
                if (pl->reads[r].key == f.parent_key) p.parent = static_cast<int>(r);
            p.cols = static_cast<int>(p.types.size());
            p.parent_cols = p.parent < 0 ? root_cols : pl->reads[static_cast<std::size_t>(p.parent)].cols;
            pl->reads.push_back(std::move(p));
            rows.push_back(f.rows);
        }
    const int n = static_cast<int>(pl->reads.size());
    // unified[p + 1]: the followers joined to parent p (-1 the root); at most one
    // of them has several rows per key (multi[p + 1]), so the joined rows of a
    // parent row are that member's rows.
    std::vector<std::vector<int>> unified(static_cast<std::size_t>(n) + 1);
    std::vector<char> multi(static_cast<std::size_t>(n) + 1, 0);
    const auto joins = [&](int parent, int i) {
        const std::size_t u = static_cast<std::size_t>(parent + 1);
        if (rows[static_cast<std::size_t>(i)] > 1.0) {
            if (multi[u]) return false;
            multi[u] = 1;
        }
        unified[u].push_back(i);
        return true;
    };
    std::vector<plan_stmt> partitioned, plain;
    for (int i = 0; i < n; ++i) {
        const planned& p = pl->reads[static_cast<std::size_t>(i)];
        if (!has_column_source(p)) continue;
        if (!joins(p.parent, i)) partitioned.push_back({plan_stmt::partitioned, p.parent, p.parent_cols, {i}, {}, {}});
    }
    std::vector<plan_stmt> as_parents;
    for (int i = 0; i < n; ++i) {
        const planned& p = pl->reads[static_cast<std::size_t>(i)];
        if (has_column_source(p)) continue;
        if (!unified[static_cast<std::size_t>(i + 1)].empty())
            as_parents.push_back({plan_stmt::unified, i, p.cols, unified[static_cast<std::size_t>(i + 1)], {}, {}});
        else if (!rs.single || !joins(-1, i))
            plain.push_back({plan_stmt::plain, -1, 0, {i}, {}, {}});
    }
    if (unified[0].empty()) pl->stmts.push_back({plan_stmt::root, -1, root_cols, {}, {}, {}});
    else pl->stmts.push_back({plan_stmt::unified, -1, root_cols, unified[0], {}, {}});
    for (auto* group : {&as_parents, &plain, &partitioned})
        for (plan_stmt& s : *group) pl->stmts.push_back(std::move(s));
    for (plan_stmt& s : pl->stmts) {
        if (s.kind != plan_stmt::partitioned) continue;
        for (int m : s.members)
            for (const source& src : pl->reads[static_cast<std::size_t>(m)].src)
                if (src.kind == source::column) s.keys.push_back(src.index);
        std::sort(s.keys.begin(), s.keys.end());
        s.keys.erase(std::unique(s.keys.begin(), s.keys.end()), s.keys.end());
    }
    return pl;
}

// Whether the root qualifies for a plan at all.
bool root_gate(const root_stat& rs) {
    return rs.seen >= min_scopes && rs.ns <= max_root_ns && rs.rows <= max_root_rows && !rs.tmpl.empty() &&
           !rs.unplannable;
}

// Whether a follower is eligible: it appeared in min_ratio of the root's scopes
// and is neither refused nor paused.
bool eligible(const root_stat& rs, const follower_stat& f) {
    return !f.failed && rs.seen >= f.paused_until && f.hits >= min_ratio * rs.scopes;
}

// Rebuilds the root's plan when what it is built from changed. A follower keyed
// on another follower needs that follower planned and sent as is.
void rebuild(root_stat& rs) {
    if (!rs.changed) return;
    rs.changed = false;
    rs.single = rs.rows <= 1.0;
    std::vector<char> on(rs.followers.size(), 0);
    bool any = false;
    for (std::size_t i = 0; i < on.size() && rs.gate; ++i) on[i] = rs.followers[i].on;
    for (std::size_t i = 0; i < on.size(); ++i) {
        const follower_stat& f = rs.followers[i];
        if (!on[i] || f.parent_key.empty()) continue;
        const auto p = std::find_if(rs.followers.begin(), rs.followers.end(),
                                    [&](const follower_stat& x) { return x.p.key == f.parent_key; });
        if (p == rs.followers.end() || !on[static_cast<std::size_t>(p - rs.followers.begin())] ||
            has_column_source(p->p))
            on[i] = 0;
    }
    for (char c : on) any = any || c;
    if (!any) {
        rs.plan = nullptr;
        return;
    }
    auto pl = build(rs, on);
    try {
        compile(*pl, rs.tmpl);
        rs.plan = std::move(pl);
    } catch (const error&) {
        rs.plan = nullptr;
    }
}

// The bit of a root text hash in prefetch_table::known.
std::pair<std::size_t, std::uint64_t> known_bit(std::uint64_t h) {
    return {static_cast<std::size_t>((h >> 6) & 63), std::uint64_t{1} << (h & 63)};
}

// Drops an eighth of a full table, the unplanned roots with the fewest scopes
// first, and recomputes the known bits of the roots that stay.
void evict(prefetch_table& t) {
    std::vector<std::pair<double, const std::string*>> victims;
    for (const auto& [text, rs] : t.roots)
        if (!rs.plan) victims.emplace_back(rs.scopes, &text);
    const std::size_t n = std::min(victims.size(), max_roots / 8);
    std::partial_sort(victims.begin(), victims.begin() + static_cast<std::ptrdiff_t>(n), victims.end());
    std::vector<std::string> drop;
    for (std::size_t i = 0; i < n; ++i) drop.push_back(*victims[i].second);
    for (const std::string& text : drop) t.roots.erase(text);
    for (auto& word : t.known) word.store(0, std::memory_order_relaxed);
    for (const auto& [text, rs] : t.roots) {
        const auto [word, bit] = known_bit(text_hash(text));
        t.known[word].fetch_or(bit, std::memory_order_relaxed);
    }
}

// The root to fold a scope into; nullptr when the table is full of planned roots.
root_stat* root_for(prefetch_table& t, const std::string& text, bool insert) {
    const auto it = t.roots.find(text);
    if (it != t.roots.end()) return &it->second;
    if (!insert) return nullptr;
    if (t.roots.size() >= max_roots) evict(t);
    if (t.roots.size() >= max_roots) return nullptr;
    const auto [word, bit] = known_bit(text_hash(text));
    t.known[word].fetch_or(bit, std::memory_order_relaxed);
    return &t.roots.emplace(text, root_stat{}).first->second;
}

// Folds a scope into the table: decays every follower of the root, counts
// those seen, refreshes their column types, marks failed ones, drops those that
// faded, and rebuilds the plan when something it is built from changed. A root
// is added only by a scope that had followers.
void learn(prefetch_table& t, prefetch_scope& sc) {
    std::vector<observed> obs;
    if (sc.trace.size() > 1) obs = observe(sc.trace);
    std::lock_guard lk(t.mu);
    t.plans += sc.plans;
    t.hits += sc.hits;
    t.misses += sc.misses;
    t.unused += sc.unused + sc.cached;
    t.failures += sc.failures;
    t.skips += sc.skips;
    if (sc.trace.empty()) return;
    const trace_entry& root = sc.trace[0];
    root_stat* found = root_for(t, root.text, !obs.empty());
    if (!found) return;
    root_stat& rs = *found;
    if (sc.timed) {
        const double ns = static_cast<double>(sc.root_ns);
        rs.ns = rs.ns > 0 ? rs.ns * 0.8 + ns * 0.2 : ns;
    }
    rs.rows = rs.seen ? rs.rows * 0.8 + root.rows * 0.2 : root.rows;
    if (!same_types(rs.types, root.cols)) {
        rs.types = info_of(root.cols);
        rs.changed = true;
    }
    if (rs.tmpl.empty() && !root.q.empty()) {
        rs.tmpl = strip_semicolon(root.q);
        rs.changed = true;
    }
    if (sc.stale) rs.changed = true;
    if (sc.unplannable && !rs.unplannable) {
        rs.unplannable = true;
        rs.changed = true;
    }
    ++rs.seen;
    rs.scopes = rs.scopes * decay + 1;
    for (follower_stat& f : rs.followers) f.hits *= decay;
    for (observed& o : obs) {
        const double avg = static_cast<double>(o.rows) / o.execs;
        const trace_entry& e = sc.trace[static_cast<std::size_t>(o.entry)];
        auto f = std::find_if(rs.followers.begin(), rs.followers.end(),
                              [&](const follower_stat& x) { return x.p.key == o.key; });
        if (f != rs.followers.end()) {
            f->hits += 1;
            const bool one = f->rows <= 1.0;
            f->rows = f->rows * 0.8 + avg * 0.2;
            if (one != (f->rows <= 1.0) && f->on) rs.changed = true;
            if (!same_types(f->p.types, e.cols)) {
                f->p.types = info_of(e.cols);
                if (f->on) rs.changed = true;
            }
            continue;
        }
        if (rs.followers.size() >= max_followers) continue;
        if (e.q.empty()) continue;  // answered from a plan dropped meanwhile
        follower_stat n;
        n.id = rs.next_id++;
        n.since = rs.seen;
        n.p.key = std::move(o.key);
        n.p.text = e.text;
        n.p.tmpl = strip_semicolon(e.q);
        n.p.params.assign(e.params.begin(), e.params.end());
        if (e.ptypes) n.p.param_types = *e.ptypes;
        n.p.src = std::move(o.src);
        n.p.types = info_of(e.cols);
        n.parent_key = std::move(o.parent_key);
        n.hits = 1;
        n.rows = avg;
        rs.followers.push_back(std::move(n));
    }
    for (const auto& [key, permanent] : sc.failed)
        for (follower_stat& f : rs.followers) {
            if (f.p.key != key) continue;
            if (permanent) f.failed = true;
            else f.paused_until = rs.seen + pause_scopes;
        }
    // The plan is built from the followers that are on: dropping another one
    // (a read whose literal changes every scope, say) leaves it as it is.
    std::erase_if(rs.followers, [&](const follower_stat& f) {
        const bool faded_out = rs.seen - f.since >= min_scopes && f.hits < faded * rs.scopes;
        if (faded_out && f.on) rs.changed = true;
        return faded_out;
    });
    rs.barren = rs.seen >= barren_after && rs.followers.empty();
    const bool gate = root_gate(rs);
    if (gate != rs.gate) {
        rs.gate = gate;
        rs.changed = true;
    }
    for (follower_stat& f : rs.followers) {
        const bool on = eligible(rs, f);
        if (on == f.on) continue;
        f.on = on;
        rs.changed = true;
    }
    if (rs.single != (rs.rows <= 1.0)) rs.changed = true;
    rebuild(rs);
}

// What the table says about a root when it runs.
struct root_view {
    std::shared_ptr<const prefetch_plan> plan;
    bool known = false;
    bool observe = true;
    bool want_tmpl = false;  // the table has no copy of the root's statement yet
};

thread_local std::uint32_t sampler = 0;

// A root not in the table is observed while its hash slot has counted fewer
// than barren_after scopes, then in one scope of barren_sample.
root_view look_up(prefetch_table& t, const std::string& text) {
    const std::uint64_t h = text_hash(text);
    const auto [word, bit] = known_bit(h);
    if (!(t.known[word].load(std::memory_order_relaxed) & bit)) {
        std::atomic<std::uint32_t>& n = t.lonely[(h >> 12) & 4095];
        if (n.load(std::memory_order_relaxed) < barren_after) {
            n.fetch_add(1, std::memory_order_relaxed);
            return {};
        }
        return {nullptr, false, sampler++ % barren_sample == 0, false};
    }
    std::lock_guard lk(t.mu);
    const auto it = t.roots.find(text);
    if (it == t.roots.end()) return {};
    root_stat& rs = it->second;
    if (rs.barren && rs.sample++ % barren_sample != 0) return {nullptr, true, false, false};
    return {rs.plan, true, true, rs.tmpl.empty()};
}

// --- plan execution --------------------------------------------------------------

// A key column as it stands for a parameter. A string column becomes a value
// of the parameter's type through cstring, which gives it the type's default
// collation, as a bind parameter has; any other column stands as is.
sql key_ref(int col, std::span<const col_info> parent) {
    const std::string ref = "__pg_k.__pg_c" + std::to_string(col + 1);
    const std::size_t c = static_cast<std::size_t>(col);
    if (c < parent.size() && parent[c].category == 'S')
        return unsafe_sql("((" + ref + ")::cstring)::" + quote_ident(parent[c].name));
    return unsafe_sql(ref);
}

// The follower's statement: root parameter slots take `root` (the root's values
// or stand-ins), column slots the parent's column.
sql resolve(const planned& p, std::span<const param> root, std::span<const col_info> parent) {
    std::vector<sql> subs(p.src.size());
    std::vector<const sql*> with(p.src.size(), nullptr);
    for (std::size_t j = 0; j < p.src.size(); ++j) {
        const source& s = p.src[j];
        if (s.kind == source::root_param) {
            if (static_cast<std::size_t>(s.index) >= root.size()) throw error(error_kind::other, "prefetch: root changed");
            subs[j] = param_sql(root[static_cast<std::size_t>(s.index)]);
        } else if (s.kind == source::column) {
            subs[j] = key_ref(s.index, parent);
        } else {
            continue;
        }
        with[j] = &subs[j];
    }
    return substitute(p.tmpl, with);
}

std::string column_list(int n) {
    std::string s;
    for (int c = 0; c < n; ++c) {
        if (c) s += ", ";
        s += "__pg_c" + std::to_string(c + 1);
    }
    return s;
}

std::span<const col_info> parent_types(const prefetch_plan& pl, const plan_stmt& st) {
    return st.parent < 0 ? std::span<const col_info>(pl.root_types)
                         : std::span<const col_info>(pl.reads[static_cast<std::size_t>(st.parent)].types);
}

// Embedded texts end on a line of their own, so a trailing line comment ends there.
void join_members(sql& s, const prefetch_plan& pl, const plan_stmt& st, std::span<const param> root,
                  std::string_view gate) {
    for (std::size_t i = 0; i < st.members.size(); ++i) {
        s += unsafe_sql(" LEFT JOIN LATERAL (SELECT row_number() OVER () AS __pg_r, __pg_x.* FROM (\n");
        s += resolve(pl.reads[static_cast<std::size_t>(st.members[i])], root, parent_types(pl, st));
        s += unsafe_sql("\n) __pg_x" + std::string(gate) + ") __pg_f" + std::to_string(i) + " ON true");
    }
}

std::string member_columns(const plan_stmt& st) {
    std::string s = "SELECT __pg_k.*";
    for (std::size_t i = 0; i < st.members.size(); ++i) s += ", __pg_f" + std::to_string(i) + ".*";
    return s;
}

// SELECT __pg_k.*, __pg_f0.*, ... FROM (SELECT row_number() OVER () AS __pg_n,
// __pg_t0.* FROM (<parent>) AS __pg_t0(__pg_c1..)) __pg_k LEFT JOIN LATERAL
// (SELECT row_number() OVER () AS __pg_r, __pg_x.* FROM (<member>) __pg_x
// WHERE __pg_k.__pg_n <= max_keys) __pg_f0 ON true ... ORDER BY __pg_k.__pg_n
sql unified_sql(const prefetch_plan& pl, const plan_stmt& st, const sql& parent, std::span<const param> root) {
    sql s = unsafe_sql(member_columns(st) + " FROM (SELECT row_number() OVER () AS __pg_n, __pg_t0.* FROM (\n");
    s += parent;
    s += unsafe_sql("\n) AS __pg_t0(" + column_list(st.parent_cols) + ")) __pg_k");
    join_members(s, pl, st, root, " WHERE __pg_k.__pg_n <= " + std::to_string(max_keys));
    s += unsafe_sql(" ORDER BY __pg_k.__pg_n");
    return s;
}

// SELECT __pg_k.*, __pg_f0.*, ... FROM (SELECT DISTINCT __pg_t.__pg_c<key>... FROM
// (<parent>) AS __pg_t(__pg_c1..) LIMIT max_keys + 1) __pg_k LEFT JOIN LATERAL ...
sql partitioned_sql(const prefetch_plan& pl, const plan_stmt& st, const sql& parent, std::span<const param> root) {
    std::string head = member_columns(st) + " FROM (SELECT DISTINCT ";
    for (std::size_t i = 0; i < st.keys.size(); ++i) {
        if (i) head += ", ";
        head += "__pg_t.__pg_c" + std::to_string(st.keys[i] + 1);
    }
    sql s = unsafe_sql(std::move(head) + " FROM (\n");
    s += parent;
    s += unsafe_sql("\n) AS __pg_t(" + column_list(st.parent_cols) + ") LIMIT " + std::to_string(max_keys + 1) +
                    ") __pg_k");
    join_members(s, pl, st, root, "");
    return s;
}

// A root parameter's stand-in while a plan's texts are rendered.
constexpr oid marker_oid = 0xFFFFFFFFu;
const std::string marker_head("\0pg-root-param", 14);

param marker(std::size_t i) { return {marker_oid, true, marker_head + std::to_string(i)}; }

batch_text text_of(const sql& s) {
    const rendered r = s.render();
    batch_text b;
    b.text = r.text;
    for (const param& p : r.params) {
        const bool m = p.type == marker_oid && p.bytes && p.bytes->compare(0, marker_head.size(), marker_head) == 0;
        b.from.push_back(m ? std::stoi(p.bytes->substr(marker_head.size())) : -1);
        b.fixed.push_back(p);
    }
    return b;
}

void add_types(std::vector<type_desc>& out, const std::vector<col_info>& types) {
    for (const col_info& c : types) out.push_back({c.id, c.name, c.category, c.kind});
}

// The result columns a statement must return, from the observed column types.
void expect(prefetch_plan& pl, plan_stmt& st) {
    const type_desc* int8 = builtin_type(20);
    const auto members = [&] {
        for (int m : st.members) {
            st.cols.push_back(*int8);
            add_types(st.cols, pl.reads[static_cast<std::size_t>(m)].types);
        }
    };
    const std::vector<col_info>& parent =
        st.parent < 0 ? pl.root_types : pl.reads[static_cast<std::size_t>(st.parent)].types;
    switch (st.kind) {
        case plan_stmt::root: add_types(st.cols, pl.root_types); break;
        case plan_stmt::plain: add_types(st.cols, pl.reads[static_cast<std::size_t>(st.members.front())].types); break;
        case plan_stmt::unified:
            st.cols.push_back(*int8);
            add_types(st.cols, parent);
            members();
            break;
        case plan_stmt::partitioned:
            for (int k : st.keys) {
                const col_info& c = parent[static_cast<std::size_t>(k)];
                st.cols.push_back({c.id, c.name, c.category, c.kind});
            }
            members();
            break;
    }
}

// Renders the plan's statements once, with the root's parameters as stand-ins,
// and the columns each must return.
void compile(prefetch_plan& pl, const sql& root) {
    pl.root_params = root.render().params.size();
    std::vector<param> markers;
    for (std::size_t i = 0; i < pl.root_params; ++i) markers.push_back(marker(i));
    std::vector<sql> marked(pl.root_params);
    std::vector<const sql*> with(pl.root_params);
    for (std::size_t i = 0; i < pl.root_params; ++i) {
        marked[i] = param_sql(markers[i]);
        with[i] = &marked[i];
    }
    const sql root_marked = substitute(root, with);
    std::vector<sql> as_is(pl.reads.size());
    pl.as_is.assign(pl.reads.size(), {});
    for (std::size_t i = 0; i < pl.reads.size(); ++i) {
        if (has_column_source(pl.reads[i])) continue;
        as_is[i] = resolve(pl.reads[i], markers, {});
        pl.as_is[i] = text_of(as_is[i]);
    }
    for (plan_stmt& st : pl.stmts) {
        const sql& parent = st.parent < 0 ? root_marked : as_is[static_cast<std::size_t>(st.parent)];
        switch (st.kind) {
            case plan_stmt::root: pl.texts.push_back(text_of(root_marked)); break;
            case plan_stmt::unified: pl.texts.push_back(text_of(unified_sql(pl, st, parent, markers))); break;
            case plan_stmt::partitioned: pl.texts.push_back(text_of(partitioned_sql(pl, st, parent, markers))); break;
            case plan_stmt::plain: pl.texts.push_back(pl.as_is[static_cast<std::size_t>(st.members.front())]); break;
        }
        expect(pl, st);
    }
}

// A batch text's parameters for this scope's root parameters.
std::vector<param> fill(const batch_text& b, std::span<const param> root) {
    std::vector<param> ps = b.fixed;
    for (std::size_t k = 0; k < ps.size(); ++k)
        if (b.from[k] >= 0) ps[k] = root[static_cast<std::size_t>(b.from[k])];
    return ps;
}

// A result of columns [first, first + n) of `from` and the given rows of it.
shared_result slice_of(const PGresult* from, int first, int n, std::span<const int> rows) {
    PGresult* raw = PQmakeEmptyPGresult(nullptr, PGRES_TUPLES_OK);
    if (!raw) throw std::bad_alloc();
    shared_result out(raw, result_deleter{});
    std::vector<PGresAttDesc> atts(static_cast<std::size_t>(n));
    for (int c = 0; c < n; ++c) {
        PGresAttDesc& a = atts[static_cast<std::size_t>(c)];
        a.name = PQfname(from, first + c);
        a.tableid = PQftable(from, first + c);
        a.columnid = PQftablecol(from, first + c);
        a.format = 1;
        a.typid = PQftype(from, first + c);
        a.typlen = PQfsize(from, first + c);
        a.atttypmod = PQfmod(from, first + c);
    }
    if (!PQsetResultAttrs(raw, n, atts.data())) throw std::bad_alloc();
    for (std::size_t t = 0; t < rows.size(); ++t)
        for (int c = 0; c < n; ++c) {
            const int row = rows[t];
            const bool null = PQgetisnull(from, row, first + c);
            if (!PQsetvalue(raw, static_cast<int>(t), c, null ? nullptr : PQgetvalue(from, row, first + c),
                            null ? -1 : PQgetlength(from, row, first + c)))
                throw std::bad_alloc();
        }
    return out;
}

param_types types_of(const prefetch_scope& sc, const planned& p) {
    return param_types(sc.plan, &p.param_types);
}

// Whether the rows are consecutive.
bool adjacent(std::span<const int> rows) {
    for (std::size_t k = 1; k < rows.size(); ++k)
        if (rows[k] != rows[0] + static_cast<int>(k)) return false;
    return true;
}

// The held result of the given rows of `whole`, `n` columns from column `first`
// on (the statement's columns there): a window of `whole` when the rows are
// consecutive, else a copy. `read` is null for the root.
std::shared_ptr<held> carve(const prefetch_scope& sc, const plan_stmt& st, const shared_result& whole, int first,
                            int n, std::span<const int> rows, const planned* read) {
    const std::span<const type_desc> cols = std::span<const type_desc>(st.cols).subspan(
        static_cast<std::size_t>(first), static_cast<std::size_t>(n));
    param_types ptypes = read ? types_of(sc, *read) : nullptr;
    if (!adjacent(rows)) {
        auto copy = slice_of(whole.get(), first, n, rows);
        return make_held(std::move(copy), cols, sc.plan, read, std::move(ptypes));
    }
    const window w{rows.empty() ? 0 : rows.front(), static_cast<int>(rows.size()), first};
    return make_held(whole, cols, sc.plan, read, std::move(ptypes), w);
}

// The parameters a member is called with for one parent row: root parameter
// slots take the root's values, column slots the parent's value at `row` of
// column `col_of(index)` of r.
template <class ColOf>
std::vector<param> member_params(const prefetch_scope& sc, const planned& p, const PGresult* r, int row, ColOf col_of) {
    std::vector<param> ps = p.params;
    for (std::size_t j = 0; j < p.src.size(); ++j) {
        const source& s = p.src[j];
        if (s.kind == source::root_param) {
            ps[j] = sc.root_params[static_cast<std::size_t>(s.index)];
        } else if (s.kind == source::column) {
            const int c = col_of(s.index);
            if (PQgetisnull(r, row, c)) ps[j].bytes.reset();
            else ps[j].bytes.emplace(PQgetvalue(r, row, c), static_cast<std::size_t>(PQgetlength(r, row, c)));
        }
    }
    return ps;
}

// The int8 value of a binary result cell.
std::int64_t int8_at(const PGresult* r, int row, int col) {
    const auto* p = reinterpret_cast<const unsigned char*>(PQgetvalue(r, row, col));
    std::uint64_t v = 0;
    for (int i = 0; i < 8; ++i) v = (v << 8) | p[i];
    return static_cast<std::int64_t>(v);
}

// Cuts the member blocks of rows [from, to) of a joined result into `copies`
// cache entries per member; `off` is the first member column. A member block
// starts with the member's row number (NULL: no row); the joined rows are the
// cross product of the members' rows, so each member's rows are its distinct
// row numbers, in order. A member's entries view the joined result.
template <class ColOf>
void slice_members(prefetch_scope& sc, const plan_stmt& st, const shared_result& whole, int from, int to, int off,
                   int copies, ColOf col_of) {
    const PGresult* r = whole.get();
    std::vector<std::pair<std::int64_t, int>> numbered;
    for (int m : st.members) {
        const planned& p = sc.plan->reads[static_cast<std::size_t>(m)];
        numbered.clear();
        for (int row = from; row < to; ++row)
            if (!PQgetisnull(r, row, off)) numbered.emplace_back(int8_at(r, row, off), row);
        std::stable_sort(numbered.begin(), numbered.end(),
                         [](const auto& a, const auto& b) { return a.first < b.first; });
        std::vector<int> hit;
        for (std::size_t k = 0; k < numbered.size(); ++k)
            if (k == 0 || numbered[k].first != numbered[k - 1].first) hit.push_back(numbered[k].second);
        read_key key = make_key(p.text, member_params(sc, p, r, from, col_of));
        push_cached(sc, std::move(key), carve(sc, st, whole, off + 1, p.cols, hit, &p), copies);
        off += 1 + p.cols;
    }
}

// Slices a unified statement: the parent's rows in row-number order, and for
// each of the first max_keys parent rows one entry per member. Returns the parent's result.
std::shared_ptr<held> slice_unified(prefetch_scope& sc, const plan_stmt& st, const shared_result& whole) {
    const PGresult* r = whole.get();
    const int np = st.parent_cols;
    std::vector<int> starts;
    for (int row = 0; row < PQntuples(r); ++row) {
        const bool fresh = row == 0 || PQgetlength(r, row, 0) != PQgetlength(r, row - 1, 0) ||
                           std::memcmp(PQgetvalue(r, row, 0), PQgetvalue(r, row - 1, 0),
                                       static_cast<std::size_t>(PQgetlength(r, row, 0))) != 0;
        if (fresh) starts.push_back(row);
    }
    const planned* read = st.parent < 0 ? nullptr : &sc.plan->reads[static_cast<std::size_t>(st.parent)];
    auto parent = carve(sc, st, whole, 1, np, starts, read);
    for (std::size_t g = 0; g < starts.size(); ++g) {
        if (int8_at(r, starts[g], 0) > max_keys) break;  // its members did not run
        const int to = g + 1 < starts.size() ? starts[g + 1] : PQntuples(r);
        slice_members(sc, st, whole, starts[g], to, 1 + np, 1, [](int col) { return 1 + col; });
    }
    return parent;
}

// The key of row `row` of the window w of r over columns col_of(keys[i]).
template <class ColOf>
std::string key_of(const PGresult* r, const window& w, int row, const std::vector<int>& keys, ColOf col_of) {
    std::string k;
    row += w.row;
    for (int key : keys) {
        const int c = w.col + col_of(key);
        const bool null = PQgetisnull(r, row, c);
        put_param(k, param{0, true, null ? std::nullopt
                                         : std::optional<std::string>(std::in_place, PQgetvalue(r, row, c),
                                                                      static_cast<std::size_t>(PQgetlength(r, row, c)))});
    }
    return k;
}

// Slices a partitioned statement: per member and distinct key, one entry per
// row of the served parent with that key (one when the parent was not served).
// More than max_keys keys: nothing.
void slice_partitioned(prefetch_scope& sc, const plan_stmt& st, const shared_result& whole, const held* parent) {
    const PGresult* r = whole.get();
    const int nk = static_cast<int>(st.keys.size());
    std::unordered_map<std::string, std::vector<int>> by_key;
    std::vector<const std::string*> order;
    const auto own = [&](int col) {
        return static_cast<int>(std::lower_bound(st.keys.begin(), st.keys.end(), col) - st.keys.begin());
    };
    for (int row = 0; row < PQntuples(r); ++row) {
        auto [it, fresh] = by_key.try_emplace(key_of(r, {}, row, st.keys, own));
        if (fresh) order.push_back(&it->first);
        it->second.push_back(row);
    }
    if (order.size() > static_cast<std::size_t>(max_keys)) return;
    std::unordered_map<std::string, int> uses;
    if (parent)
        for (int row = 0; row < rows_in(parent->r.get(), parent->w); ++row)
            ++uses[key_of(parent->r.get(), parent->w, row, st.keys, [](int c) { return c; })];
    for (const std::string* k : order) {
        const std::vector<int>& rows = by_key[*k];
        // One lateral run per key: its rows are contiguous, or the key is left out.
        if (rows.back() - rows.front() + 1 != static_cast<int>(rows.size())) continue;
        const auto u = uses.find(*k);
        const int copies = !parent ? 1 : u == uses.end() ? 0 : u->second;
        slice_members(sc, st, whole, rows.front(), rows.back() + 1, nk, copies, own);
    }
}

// Counts a failed batch. A refused statement (read-only, class 42) leaves the
// plan for good, a syntax error makes the root unplannable, other statement
// errors pause its reads; a connection failure only costs this scope. The
// statements after it did not run.
void note_failure(prefetch_scope& sc, std::size_t failed, const error& e) {
    ++sc.failures;
    if (failed >= sc.plan->stmts.size() || e.kind == error_kind::broken || e.kind == error_kind::connect_failed ||
        e.kind == error_kind::stale_statement)
        return;
    if (e.sqlstate == "42601") sc.unplannable = true;
    const bool permanent = e.kind == error_kind::read_only || e.sqlstate.starts_with("42");
    const plan_stmt& st = sc.plan->stmts[failed];
    for (int m : st.members) sc.failed.emplace_back(sc.plan->reads[static_cast<std::size_t>(m)].key, permanent);
    if (st.kind == plan_stmt::unified && st.parent >= 0)
        sc.failed.emplace_back(sc.plan->reads[static_cast<std::size_t>(st.parent)].key, permanent);
}

// Whether a batch result has the columns the plan expects.
bool fits(const PGresult* r, const plan_stmt& st) {
    if (PQnfields(r) != static_cast<int>(st.cols.size())) return false;
    for (std::size_t c = 0; c < st.cols.size(); ++c)
        if (PQftype(r, static_cast<int>(c)) != st.cols[c].id) return false;
    return true;
}

// Puts what a batch returned into the cache; returns the root's result when the
// batch brought it. A result without the planned columns is dropped and the
// plan rebuilt. Never throws.
std::shared_ptr<held> absorb(prefetch_scope& sc, landed_batch& b) {
    if (b.failure) note_failure(sc, b.failed, *b.failure);
    std::shared_ptr<held> root;
    std::vector<const held*> served(sc.plan->reads.size(), nullptr);
    for (std::size_t i = 0; i < b.results.size(); ++i) {
        if (!b.results[i]) continue;
        const plan_stmt& st = sc.plan->stmts[i];
        if (!fits(b.results[i].get(), st)) {
            ++sc.failures;
            sc.stale = true;
            continue;
        }
        try {
            shared_result whole(b.results[i].release(), result_deleter{});
            if (st.kind == plan_stmt::root) {
                root = make_held(std::move(whole), st.cols, sc.plan, nullptr, nullptr);
            } else if (st.kind == plan_stmt::partitioned) {
                const held* parent = st.parent < 0 ? root.get() : served[static_cast<std::size_t>(st.parent)];
                slice_partitioned(sc, st, whole, parent);
            } else if (st.kind == plan_stmt::unified) {
                std::shared_ptr<held> parent = slice_unified(sc, st, whole);
                if (st.parent < 0) {
                    root = std::move(parent);
                } else {
                    served[static_cast<std::size_t>(st.parent)] = parent.get();
                    push_cached(sc, std::move(sc.as_is_keys[static_cast<std::size_t>(st.parent)]), std::move(parent));
                }
            } else {
                const std::size_t m = static_cast<std::size_t>(st.members.front());
                auto h = make_held(std::move(whole), st.cols, sc.plan, &sc.plan->reads[m],
                                   types_of(sc, sc.plan->reads[m]));
                served[m] = h.get();
                push_cached(sc, std::move(sc.as_is_keys[m]), std::move(h));
            }
        } catch (...) {
            ++sc.failures;
        }
    }
    return root;
}

thread_local std::unique_ptr<prefetch_scope> spare;

}  // namespace

void prefetch_scope::clear() {
    trace.clear();
    forget();
    cached = 0;
    failed.clear();
    plans = hits = misses = unused = failures = skips = 0;
    planned = known = want_tmpl = stale = unplannable = timed = false;
    root_ns = 0;
    plan.reset();
    root_params.clear();
    as_is_keys.clear();
    served.reset();
}

void recycle(std::unique_ptr<prefetch_scope> sc) {
    if (!spare) spare = std::move(sc);
}

void prefetch_counts(prefetch_table& t, pool_stats& out) {
    std::lock_guard lk(t.mu);
    out.prefetch_plans = t.plans;
    out.prefetch_hits = t.hits;
    out.prefetch_misses = t.misses;
    out.prefetch_unused = t.unused;
    out.prefetch_failures = t.failures;
    out.prefetch_skips = t.skips;
}

}  // namespace detail

detail::reply conn::prefetched(const sql& q, rendered& r, detail::check_fn check, bool pins) {
    using namespace detail;
    if (!check || keeps(pins) || !candidate(r.text) || sending()) {
        end_scope();
        return run_unscoped(r, check, pins);
    }
    if (pf_idle_) return run_unscoped(r, check, false);
    root_view v;
    if (!pf_ || pf_->trace.empty()) {
        v = look_up(prefetch_of(state()), r.text);
        if (!v.observe) {
            pf_idle_ = true;
            return run_unscoped(r, check, false);
        }
    }
    if (!pf_) pf_ = spare ? std::move(spare) : std::make_unique<prefetch_scope>();
    prefetch_scope& sc = *pf_;
    const bool traced = sc.trace.size() < max_trace;
    if (sc.cached) {
        if (cache_entry* e = find_cached(sc, r.text, r.params)) {
            std::shared_ptr<held> h = serve_cached(sc, *e);
            if (auto bad = check(h->cols)) throw error(error_kind::other, *bad, {}, r.text);
            ++sc.hits;
            reply rp = view_of(h);
            if (traced) record_held(sc, r, nullptr, h);  // a planned read: its statement is known
            return rp;
        }
    }
    const bool root = sc.trace.empty();
    std::shared_ptr<const prefetch_plan> pl;
    if (root) {
        sc.known = v.known;
        sc.want_tmpl = v.want_tmpl;
        pl = std::move(v.plan);
    } else if (sc.planned) {
        // An unpredicted read may write: what the scope prefetched is dropped.
        ++sc.misses;
        drop_cache(sc);
    }
    param_types types;
    reply rp;
    // Only an unplanned root the table knows is timed: its own time, not a batch's.
    sc.timed = root && sc.known && !pl;
    const auto t0 = sc.timed ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
    try {
        if (pl) {
            sc.plan = std::move(pl);
            rp = run_plan(*sc.plan, q, r, check);
        } else {
            rp = run_unscoped(r, check, false, root ? nullptr : &types);
        }
    } catch (...) {
        if (root) end_scope();
        throw;
    }
    if (sc.timed) sc.root_ns = (std::chrono::steady_clock::now() - t0).count();
    if (sc.served) {
        const std::shared_ptr<held> h = std::move(sc.served);
        record_held(sc, r, sc.want_tmpl ? &q : nullptr, h);
        return rp;
    }
    if (!traced) return rp;
    return record(sc, r, root && !sc.want_tmpl ? nullptr : &q, std::move(rp), std::move(types));
}

// The root and the plan's statements go in one batch on one session; the
// root's result is served from it. When the batch does not bring the root, the
// root runs as the program sent it.
detail::reply conn::run_plan(const detail::prefetch_plan& pl, const sql&, const rendered& r, detail::check_fn check) {
    using namespace detail;
    prefetch_scope& sc = *pf_;
    if (r.params.size() != pl.root_params) return run_unscoped(r, check, false);
    sc.root_params.assign(r.params.begin(), r.params.end());
    sc.as_is_keys.assign(pl.reads.size(), {});
    for (std::size_t i = 0; i < pl.reads.size(); ++i)
        if (!pl.as_is[i].text.empty()) sc.as_is_keys[i] = make_key(pl.as_is[i].text, fill(pl.as_is[i], r.params));
    std::vector<batch_item> items;
    items.reserve(pl.texts.size());
    for (const batch_text& b : pl.texts) items.push_back({b.text, fill(b, r.params)});
    std::shared_ptr<speculation> x;
    try {
        x = speculate(*p_->st_, p_->opts_, std::move(items), sc.plan);
    } catch (const error&) {
        ++sc.failures;
        return run_unscoped(r, check, false);
    }
    if (!x) {
        ++sc.skips;
        return run_unscoped(r, check, false);
    }
    sc.planned = true;
    ++sc.plans;
    landed_batch b = land_speculation(*x);
    std::shared_ptr<held> served = absorb(sc, b);
    if (!served) return run_unscoped(r, check, false);
    if (auto bad = check(served->cols)) throw error(error_kind::other, *bad, {}, r.text);
    sc.served = served;
    return view_of(served);
}

void conn::close_scope() {
    detail::prefetch_scope& sc = *pf_;
    if (!sc.active()) return;
    const bool learn = sc.trace.size() > 1 || sc.known || sc.plans || sc.failures || sc.skips || sc.misses ||
                       sc.unused || sc.cached;
    if (learn) {
        try {
            detail::learn(detail::prefetch_of(state()), sc);
        } catch (...) {
            // Learning is best effort; the scope ends regardless.
        }
    }
    sc.clear();
}

}  // namespace pg
