// Markdown report of a results CSV: one table per scenario, plus two cross-scenario
// matrices (ops/s and p99 by thread count).
#include <algorithm>
#include <cstdio>
#include <fstream>
#include <map>
#include <set>
#include <sstream>

#include "harness.hpp"

namespace bench {
namespace {

struct Row {
    std::string scenario;
    int threads = 0, pool = 0;
    std::string ops, p50, p99, p999, extra;
};

std::vector<std::string> split(const std::string& line) {
    std::vector<std::string> f;
    std::string cur;
    std::istringstream in(line);
    while (std::getline(in, cur, ',')) f.push_back(cur);
    if (!line.empty() && line.back() == ',') f.emplace_back();
    f.resize(8);
    return f;
}

std::vector<Row> read_rows(const std::string& path) {
    std::ifstream in(path);
    if (!in) throw std::runtime_error("cannot open " + path);
    std::vector<Row> rows;
    std::string line;
    std::getline(in, line);  // header
    while (std::getline(in, line)) {
        if (line.empty()) continue;
        const auto f = split(line);
        rows.push_back({f[0], std::atoi(f[1].c_str()), std::atoi(f[2].c_str()), f[3], f[4], f[5], f[6], f[7]});
    }
    return rows;
}

std::string dash(const std::string& s) { return s.empty() ? "-" : s; }

void scenario_tables(const std::vector<Row>& rows) {
    std::vector<std::string> order;
    for (const auto& r : rows)
        if (std::find(order.begin(), order.end(), r.scenario) == order.end()) order.push_back(r.scenario);
    for (const auto& name : order) {
        std::printf("### %s\n\n| threads | pool | ops/s | p50 us | p99 us | p99.9 us | extra |\n|---:|---:|---:|---:|---:|---:|---|\n",
                    name.c_str());
        for (const auto& r : rows)
            if (r.scenario == name)
                std::printf("| %d | %s | %s | %s | %s | %s | %s |\n", r.threads, r.pool ? std::to_string(r.pool).c_str() : "-",
                            dash(r.ops).c_str(), dash(r.p50).c_str(), dash(r.p99).c_str(), dash(r.p999).c_str(),
                            r.extra.c_str());
        std::printf("\n");
    }
}

// Rows: scenario (with /P=pool when pooled); columns: thread counts.
void matrix(const std::vector<Row>& rows, const char* title, std::string Row::*field) {
    std::set<int> cols;
    std::vector<std::string> keys;
    std::map<std::string, std::map<int, std::string>> cell;
    for (const auto& r : rows) {
        if (r.scenario == "v1_cached_flush") continue;
        const std::string key = r.pool && r.scenario != "v1_select_hold" ? r.scenario + " P=" + std::to_string(r.pool) : r.scenario;
        if (!cell.count(key)) keys.push_back(key);
        cell[key][r.threads] = dash(r.*field);
        cols.insert(r.threads);
    }
    std::printf("### %s\n\n| scenario |", title);
    for (int c : cols) std::printf(" T=%d |", c);
    std::printf("\n|---|");
    for (size_t i = 0; i < cols.size(); ++i) std::printf("---:|");
    std::printf("\n");
    for (const auto& k : keys) {
        std::printf("| %s |", k.c_str());
        for (int c : cols) std::printf(" %s |", cell[k].count(c) ? cell[k][c].c_str() : "");
        std::printf("\n");
    }
    std::printf("\n");
}

}  // namespace

int summarize(const std::string& csv_path) {
    const auto rows = read_rows(csv_path);
    matrix(rows, "Throughput overview (ops/s)", &Row::ops);
    matrix(rows, "Latency overview (p99, us)", &Row::p99);
    std::printf("## Per-scenario tables\n\n");
    scenario_tables(rows);
    return 0;
}

}  // namespace bench
