#pragma once
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string_view>
#include <type_traits>
#include <vector>

#include "codec.hpp"
#include "error.hpp"

namespace pg {

namespace detail {

// FNV-1a: the same value in every process, build and platform, unlike std::hash.
inline std::uint64_t fnv(std::uint64_t h, std::string_view b) {
    for (const unsigned char c : b) {
        h ^= c;
        h *= 0x100000001b3ULL;
    }
    return h;
}

inline std::uint64_t fnv_u64(std::uint64_t h, std::uint64_t v) {
    char b[8];
    for (int i = 0; i < 8; ++i) b[i] = static_cast<char>(v >> (56 - 8 * i));
    return fnv(h, {b, 8});
}

// Adds one key component. Integers are widened to 64 bits, so that int and long
// keys land on the same shard; text is length-prefixed, so that ("ab", "c") and
// ("a", "bc") differ; anything else hashes its binary wire encoding.
template <class T>
std::uint64_t add_component(std::uint64_t h, const T& v) {
    if constexpr (std::is_integral_v<T> && !std::is_same_v<T, bool>) {
        return fnv_u64(h, static_cast<std::uint64_t>(static_cast<std::int64_t>(v)));
    } else if constexpr (std::is_convertible_v<const T&, std::string_view>) {
        const std::string_view s(v);
        return fnv(fnv_u64(h, s.size()), s);
    } else {
        const param p = to_param(v);
        if (!p.bytes) throw error(error_kind::other, "a shard key component is NULL");
        return fnv(fnv_u64(h, p.bytes->size()), *p.bytes);
    }
}

// splitmix64's finalizer: spreads FNV's weak low bits over the whole word.
inline std::uint64_t finalize(std::uint64_t z) {
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
    return z ^ (z >> 31);
}

// Jump consistent hash (Lamping and Veach, 2014): going from n to n + 1 shards
// moves only the keys that land on the new shard, 1/(n + 1) of them.
inline std::size_t jump(std::uint64_t key, std::size_t n) {
    std::int64_t b = -1, j = 0;
    while (j < static_cast<std::int64_t>(n)) {
        b = j;
        key = key * 2862933555777941757ULL + 1;
        j = static_cast<std::int64_t>(static_cast<double>(b + 1) *
                                      (static_cast<double>(std::int64_t{1} << 31) / static_cast<double>((key >> 33) + 1)));
    }
    return static_cast<std::size_t>(b);
}

}  // namespace detail

// The shard of a key among n, the same in every process. Shards are numbered by
// their place in the configuration: new shards go at the end, never in between.
template <class... K>
    requires(sizeof...(K) > 0)
std::size_t shard_of(std::size_t n, const K&... key) {
    std::uint64_t h = 0xcbf29ce484222325ULL;
    ((h = detail::add_component(h, key)), ...);
    return detail::jump(detail::finalize(h), n);
}

// One T per shard: a pool per database server, a writeback per pool. Two
// sharded sets of the same size route a key to the same index, so
// pools.of(id) and writebacks.of(id) belong together. A unit of work (a
// transaction, a posting) stays on one shard; nothing spans shards.
template <class T>
class sharded {
public:
    // One T per argument, made as T(arg).
    template <class A>
        requires std::constructible_from<T, const A&>
    explicit sharded(const std::vector<A>& args) {
        if (args.empty()) throw error(error_kind::other, "sharded: needs at least one shard");
        for (const A& a : args) items_.push_back(std::make_unique<T>(a));
    }

    // n of T, made by make(i), which returns std::unique_ptr<T>.
    template <class F>
        requires std::convertible_to<std::invoke_result_t<F&, std::size_t>, std::unique_ptr<T>>
    sharded(std::size_t n, F make) {
        if (n == 0) throw error(error_kind::other, "sharded: needs at least one shard");
        for (std::size_t i = 0; i < n; ++i) items_.push_back(make(i));
    }

    template <class... K>
    T& of(const K&... key) {
        return *items_[shard_of(items_.size(), key...)];
    }

    T& operator[](std::size_t i) { return *items_.at(i); }
    std::size_t size() const { return items_.size(); }

private:
    std::vector<std::unique_ptr<T>> items_;
};

}  // namespace pg
