// Sharded in-memory key/value store (u64 id -> string), one spinlock per shard.
#pragma once

#include <atomic>
#include <cstddef>
#include <string>
#include <unordered_map>

#include "types.h"

struct alignas(64) Shard {
    std::atomic_flag lk = ATOMIC_FLAG_INIT;
    std::unordered_map<u64, std::string> m;
    void lock() {
        while (lk.test_and_set(std::memory_order_acquire))
            while (lk.test(std::memory_order_relaxed)) __builtin_ia32_pause();
    }
    void unlock() { lk.clear(std::memory_order_release); }
};

inline constexpr size_t kShards = 256;
extern Shard g_shards[kShards];  // defined in store.cpp

inline Shard& shard_for(u64 id) { return g_shards[(id * 0x9E3779B97F4A7C15ull) >> 56]; }

struct Guard {
    Shard& s;
    explicit Guard(Shard& sh) : s(sh) { s.lock(); }
    ~Guard() { s.unlock(); }
};
