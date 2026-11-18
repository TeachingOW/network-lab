// Standalone store regression tests; no Linux networking dependencies.
#include <cassert>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <new>
#include <random>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

static bool fail_alloc = false;
static void* test_realloc(void* p, size_t n) { return fail_alloc ? nullptr : std::realloc(p, n); }
static void* test_aligned_alloc(size_t a, size_t n) { return fail_alloc ? nullptr : std::aligned_alloc(a, n); }
#define realloc test_realloc
#define aligned_alloc test_aligned_alloc
#include "store.h"
#undef aligned_alloc
#undef realloc

Shard g_shards[kShards];

static void check(const Table& t, u64 key, const std::string& expected) {
    const Slot* s = t.find(key);
    assert(s && s->len == expected.size());
    assert(std::string_view(s->data(), s->len) == expected);
}

static void overwrites() {
    Table t;
    for (u64 i = 0; i < 48; i++) t.put(i, "a", 1);
    assert(t.capacity() == 64);
    for (u64 i = 0; i < 48; i++) t.put(i, "b", 1);
    assert(t.size() == 48 && t.capacity() == 64);
    t.put(48, "c", 1);
    assert(t.size() == 49 && t.capacity() == 128);
    for (size_t n : {size_t(0), size_t(48), size_t(49), size_t(200), size_t(50), size_t(1)}) {
        std::string v(n, '\0');
        t.put(0, v.data(), v.size());
        check(t, 0, v);
    }
    size_t allocations = 0;
    uint32_t cap = 0;
    for (size_t n = 49; n <= 8192; n++) {
        std::string v(n, 'x');
        t.put(0, v.data(), v.size());
        check(t, 0, v);
        uint32_t next = t.find(0)->cap;
        allocations += next != cap;
        cap = next;
        assert(next >= n && next <= n * 3 / 2);
    }
    assert(allocations < 20);
}

static void collisions() {
    // All homes are bucket 63 in the initial 64-slot table: wraparound and shifts.
    std::vector<u64> keys;
    for (u64 k = 0; keys.size() < 48; k++)
        if (((k * kHashMul) << 8) >> 58 == 63) keys.push_back(k);
    for (size_t victim = 0; victim < keys.size(); victim++) {
        Table t;
        for (u64 k : keys) t.put(k, "long value that exercises heap ownership during backward shifts", 63);
        assert(t.erase(keys[victim]));
        assert(!t.find(keys[victim]) && !t.erase(keys[victim]));
        for (size_t i = 0; i < keys.size(); i++) if (i != victim) assert(t.find(keys[i]));
    }
}

static void failures() {
    Table t;
    fail_alloc = true;
    try { t.put(1, "a", 1); assert(false); } catch (const std::bad_alloc&) {}
    fail_alloc = false;
    assert(t.size() == 0 && t.capacity() == 0);
    t.put(1, "old", 3);
    std::string big(100, 'x');
    fail_alloc = true;
    try { t.put(1, big.data(), big.size()); assert(false); } catch (const std::bad_alloc&) {}
    try { t.put(2, big.data(), big.size()); assert(false); } catch (const std::bad_alloc&) {}
    fail_alloc = false;
    check(t, 1, "old");
    assert(t.size() == 1 && !t.find(2));
    t.put(1, big.data(), big.size());
    big += 'y';
    fail_alloc = true;
    try { t.put(1, big.data(), big.size()); assert(false); } catch (const std::bad_alloc&) {}
    fail_alloc = false;
    check(t, 1, std::string(100, 'x'));
    for (u64 k = 2; k <= 48; k++) t.put(k, "a", 1);
    fail_alloc = true;
    try { t.put(49, "a", 1); assert(false); } catch (const std::bad_alloc&) {}
    fail_alloc = false;
    assert(t.size() == 48 && t.capacity() == 64 && !t.find(49));
    check(t, 1, std::string(100, 'x'));
    try { t.put(1, "", Slot::kEmpty); assert(false); } catch (const std::length_error&) {}
    check(t, 1, std::string(100, 'x'));
}

static void randomized() {
    Table t;
    std::unordered_map<u64, std::string> ref;
    std::mt19937_64 rng(42);
    for (int i = 0; i < 100000; i++) {
        u64 k = rng() % 4096;
        switch (rng() % 3) {
            case 0: {
                std::string value(rng() % 256, char(rng()));
                t.put(k, value.data(), value.size()); ref[k] = value; break;
            }
            case 1: assert(t.erase(k) == bool(ref.erase(k))); break;
            case 2:
                if (ref.count(k)) check(t, k, ref.at(k)); else assert(!t.find(k));
        }
        assert(t.size() == ref.size());
    }
    t.put(~u64(0), "max", 3); check(t, ~u64(0), "max");
    for (const auto& [k, v] : ref) check(t, k, v);
}

static void concurrent() {
    std::vector<std::thread> threads;
    for (u64 worker = 0; worker < 4; worker++) threads.emplace_back([worker] {
        for (u64 i = 0; i < 10000; i++) {
            u64 k = worker * 10000 + i;
            Shard& s = shard_for(k);
            Guard g(s);
            s.m.put(k, "v", 1);
            check(s.m, k, "v");
            if (i % 2) assert(s.m.erase(k));
        }
    });
    for (auto& t : threads) t.join();
    size_t total = 0;
    for (auto& s : g_shards) total += s.m.size();
    assert(total == 20000);
}

int main() { overwrites(); collisions(); failures(); randomized(); concurrent(); }
