// The listener thread of a writeback: one libpq connection of its own (not
// from the pool) that LISTENs on the watched-column channel and hands the
// named keys to the writer, which reconciles them.
#include <libpq-fe.h>
#include <poll.h>

#include <algorithm>
#include <cstdint>

#include "libpg_schema.hpp"
#include "writeback.hpp"

namespace pg::detail {
namespace {

// ---- the payload: {"t":"table","c":"col","k":[["k1",...],...]} -------------

struct json_reader {
    std::string_view s;
    std::size_t i = 0;

    void space() {
        while (i < s.size() && (s[i] == ' ' || s[i] == '\n' || s[i] == '\t' || s[i] == '\r')) ++i;
    }
    bool eat(char c) {
        space();
        if (i >= s.size() || s[i] != c) return false;
        ++i;
        return true;
    }
    std::optional<unsigned> hex4() {
        if (i + 4 > s.size()) return std::nullopt;
        unsigned v = 0;
        for (int k = 0; k < 4; ++k) {
            const char c = s[i++];
            const int d = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
            if (d < 0) return std::nullopt;
            v = v * 16 + static_cast<unsigned>(d);
        }
        return v;
    }
    std::optional<std::string> string();
    std::optional<std::vector<std::string>> string_array();
};

void put_utf8(std::string& out, unsigned cp) {
    if (cp < 0x80) {
        out += static_cast<char>(cp);
    } else if (cp < 0x800) {
        out += static_cast<char>(0xc0 | (cp >> 6));
        out += static_cast<char>(0x80 | (cp & 0x3f));
    } else if (cp < 0x10000) {
        out += static_cast<char>(0xe0 | (cp >> 12));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3f));
        out += static_cast<char>(0x80 | (cp & 0x3f));
    } else {
        out += static_cast<char>(0xf0 | (cp >> 18));
        out += static_cast<char>(0x80 | ((cp >> 12) & 0x3f));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3f));
        out += static_cast<char>(0x80 | (cp & 0x3f));
    }
}

// The code point of a \u escape (the "\u" already read), joining surrogate pairs.
std::optional<unsigned> unicode_escape(json_reader& r) {
    const auto hi = r.hex4();
    if (!hi || *hi < 0xd800 || *hi > 0xdbff) return hi;
    if (r.s.substr(r.i, 2) != "\\u") return std::nullopt;
    r.i += 2;
    const auto lo = r.hex4();
    if (!lo || *lo < 0xdc00 || *lo > 0xdfff) return std::nullopt;
    return 0x10000 + ((*hi - 0xd800) << 10) + (*lo - 0xdc00);
}

bool escape(json_reader& r, std::string& out) {
    if (r.i >= r.s.size()) return false;
    const char c = r.s[r.i++];
    static constexpr std::string_view from = "\"\\/bfnrt", to = "\"\\/\b\f\n\r\t";
    if (const auto k = from.find(c); k != std::string_view::npos) {
        out += to[k];
        return true;
    }
    if (c != 'u') return false;
    const auto cp = unicode_escape(r);
    if (cp) put_utf8(out, *cp);
    return cp.has_value();
}

std::optional<std::string> json_reader::string() {
    if (!eat('"')) return std::nullopt;
    std::string out;
    while (i < s.size() && s[i] != '"') {
        const char c = s[i++];
        if (c != '\\') out += c;
        else if (!escape(*this, out)) return std::nullopt;
    }
    if (i >= s.size()) return std::nullopt;
    ++i;
    return out;
}

std::optional<std::vector<std::string>> json_reader::string_array() {
    if (!eat('[')) return std::nullopt;
    std::vector<std::string> out;
    if (eat(']')) return out;
    do {
        auto v = string();
        if (!v) return std::nullopt;
        out.push_back(std::move(*v));
    } while (eat(','));
    if (!eat(']')) return std::nullopt;
    return out;
}

bool keys_field(json_reader& r, notice& n) {
    if (!r.eat('[')) return false;
    if (r.eat(']')) return true;
    do {
        auto k = r.string_array();
        if (!k) return false;
        n.keys.push_back(std::move(*k));
    } while (r.eat(','));
    return r.eat(']');
}

bool all_field(json_reader& r, notice& n) {
    r.space();
    for (std::string_view lit : {"true", "false"}) {
        if (r.s.substr(r.i, lit.size()) != lit) continue;
        r.i += lit.size();
        n.all = lit == "true";
        return true;
    }
    return false;
}

bool field(json_reader& r, notice& n) {
    const auto name = r.string();
    if (!name || !r.eat(':')) return false;
    if (*name == "k") return keys_field(r, n);
    if (*name == "all") return all_field(r, n);
    auto v = r.string();
    if (!v) return false;
    if (*name == "t") n.table = std::move(*v);
    else if (*name == "c") n.column = std::move(*v);
    return true;
}

// ---- the connection --------------------------------------------------------

using conn_ptr = std::unique_ptr<PGconn, decltype(&PQfinish)>;

conn_ptr connect_and_listen(wb_state& w) {
    const std::string& dsn = w.o.direct_dsn.empty() ? w.p.options().dsn : w.o.direct_dsn;
    conn_ptr c(PQconnectdb(dsn.c_str()), PQfinish);
    if (PQstatus(c.get()) != CONNECTION_OK) return conn_ptr(nullptr, PQfinish);
    const std::string sql = "LISTEN " + std::string(watch_channel);
    PGresult* r = PQexec(c.get(), sql.c_str());
    const bool ok = PQresultStatus(r) == PGRES_COMMAND_OK;
    PQclear(r);
    if (!ok) return conn_ptr(nullptr, PQfinish);
    return c;
}

// Waits on the connection (or only the wake pipe without one, up to `ms`);
// false once the writeback stops.
bool wait(wb_state& w, PGconn* c, int ms) {
    pollfd fds[2] = {{w.wake_pipe[0], POLLIN, 0}, {c ? PQsocket(c) : -1, POLLIN, 0}};
    poll(fds, c ? 2 : 1, ms);
    return !w.listen_stop.load();
}

void handle(wb_state& w, const PGnotify& n) {
    if (w.own_pid(n.be_pid)) {
        w.ignored.fetch_add(1);
        return;
    }
    auto parsed = parse_notice(n.extra);
    if (!parsed) return;
    counter* c = w.find_counter(parsed->table, parsed->column);
    if (!c || !c->watched.load()) return;
    {
        std::lock_guard lk(w.m);
        w.notified.push_back({c, std::move(parsed->keys), parsed->all});
        w.wake = true;
    }
    w.cv.notify_one();
}

// Reads notifications until the connection is lost or the writeback stops.
void pump(wb_state& w, PGconn* c) {
    while (wait(w, c, -1)) {
        if (!PQconsumeInput(c) || PQstatus(c) != CONNECTION_OK) return;
        while (PGnotify* n = PQnotifies(c)) {
            handle(w, *n);
            PQfreemem(n);
        }
    }
}

void request_full_reconcile(wb_state& w) {
    {
        std::lock_guard lk(w.m);
        w.full_due = true;
        w.wake = true;
    }
    w.cv.notify_one();
}

}  // namespace

std::optional<notice> parse_notice(std::string_view payload) {
    json_reader r{payload};
    notice n;
    if (!r.eat('{')) return std::nullopt;
    do {
        if (!field(r, n)) return std::nullopt;
    } while (r.eat(','));
    if (!r.eat('}') || n.table.empty() || n.column.empty()) return std::nullopt;
    return n;
}

// Notifications sent while the listener was not connected are lost: every
// (re)connect is followed by a full reconcile of the watched counters.
void listen_loop(wb_state& w) {
    int retry_ms = 100;
    while (!w.listen_stop.load()) {
        conn_ptr c = connect_and_listen(w);
        if (!c) {
            if (!wait(w, nullptr, retry_ms)) return;
            retry_ms = std::min(retry_ms * 2, 5000);
            continue;
        }
        retry_ms = 100;
        request_full_reconcile(w);
        pump(w, c.get());
    }
}

}  // namespace pg::detail
