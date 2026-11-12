// Sharded in-memory key/value store (u64 id -> bytes), one spinlock per shard.
//
// Each shard is an open-addressing table with linear probing: one 64-byte slot per item holding the
// key, the length and, for values up to 48 bytes, the value itself. A lookup touches one cache line
// (std::unordered_map touched a bucket, a node and the string's heap buffer). Deletion shifts the
// following entries back, so there are no tombstones.
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdlib>
#include <cstring>

#include "types.h"

inline constexpr u64 kHashMul = 0x9E3779B97F4A7C15ull;

struct alignas(64) Slot {
    static constexpr uint32_t kEmpty = ~0u;
    static constexpr size_t kInline = 48;
    u64 key;
    uint32_t len;
    uint32_t cap;  // kEmpty: unused slot; 0: value inline; otherwise the heap buffer's capacity
    union { char inl[kInline]; char* heap; };
    const char* data() const { return cap ? heap : inl; }
};
static_assert(sizeof(Slot) == 64);

class Table {
public:
    ~Table() { clear(); }
    size_t size() const { return n_; }

    const Slot* find(u64 key) const {
        if (!n_) return nullptr;
        for (u64 i = home(key);; i = (i + 1) & mask_) {
            const Slot& s = s_[i];
            if (s.cap == Slot::kEmpty) return nullptr;
            if (s.key == key) return &s;
        }
    }

    void put(u64 key, const char* p, size_t len) {
        if ((n_ + 1) * 4 > (mask_ + 1) * 3 || !s_) grow();  // keep the load factor at or below 3/4
        u64 i = home(key);
        for (;; i = (i + 1) & mask_) {
            Slot& s = s_[i];
            if (s.cap == Slot::kEmpty) { s.key = key; s.cap = 0; n_++; break; }
            if (s.key == key) break;
        }
        Slot& s = s_[i];
        if (len <= Slot::kInline) {
            if (s.cap) { free(s.heap); s.cap = 0; }
            memcpy(s.inl, p, len);
        } else {
            if (s.cap < len) {
                if (s.cap) free(s.heap);
                s.heap = (char*)malloc(len);
                s.cap = (uint32_t)len;
            }
            memcpy(s.heap, p, len);
        }
        s.len = (uint32_t)len;
    }

    bool erase(u64 key) {
        if (!n_) return false;
        u64 i = home(key);
        for (;; i = (i + 1) & mask_) {
            if (s_[i].cap == Slot::kEmpty) return false;
            if (s_[i].key == key) break;
        }
        if (s_[i].cap) free(s_[i].heap);
        // backward-shift deletion: move each following entry into the hole unless its home lies
        // (cyclically) after the hole, in which case moving it would make it unreachable
        for (u64 j = i;;) {
            j = (j + 1) & mask_;
            if (s_[j].cap == Slot::kEmpty) break;
            u64 h = home(s_[j].key);
            bool stays = i <= j ? (i < h && h <= j) : (i < h || h <= j);
            if (stays) continue;
            s_[i] = s_[j];
            i = j;
        }
        s_[i].cap = Slot::kEmpty;
        n_--;
        return true;
    }

private:
    Slot* s_ = nullptr;
    u64 mask_ = 0;
    unsigned bits_ = 0;
    size_t n_ = 0;

    // The shard is picked by the top 8 bits of key * kHashMul; the slot by the bits below those.
    u64 home(u64 key) const { return ((key * kHashMul) << 8) >> (64 - bits_); }

    void grow() {
        Slot* old = s_;
        u64 old_n = s_ ? mask_ + 1 : 0;
        bits_ = s_ ? bits_ + 1 : 6;
        mask_ = (1ull << bits_) - 1;
        s_ = (Slot*)aligned_alloc(64, (mask_ + 1) * sizeof(Slot));
        for (u64 i = 0; i <= mask_; i++) s_[i].cap = Slot::kEmpty;
        for (u64 i = 0; i < old_n; i++) {
            if (old[i].cap == Slot::kEmpty) continue;
            u64 j = home(old[i].key);
            while (s_[j].cap != Slot::kEmpty) j = (j + 1) & mask_;
            s_[j] = old[i];
        }
        free(old);
    }
    void clear() {
        if (!s_) return;
        for (u64 i = 0; i <= mask_; i++)
            if (s_[i].cap != Slot::kEmpty && s_[i].cap) free(s_[i].heap);
        free(s_);
        s_ = nullptr;
    }
};

struct alignas(64) Shard {
    std::atomic_flag lk = ATOMIC_FLAG_INIT;
    Table m;
    void lock() {
        while (lk.test_and_set(std::memory_order_acquire))
            while (lk.test(std::memory_order_relaxed)) __builtin_ia32_pause();
    }
    void unlock() { lk.clear(std::memory_order_release); }
};

inline constexpr size_t kShards = 256;
extern Shard g_shards[kShards];  // defined in store.cpp

inline Shard& shard_for(u64 id) { return g_shards[(id * kHashMul) >> 56]; }

struct Guard {
    Shard& s;
    explicit Guard(Shard& sh) : s(sh) { s.lock(); }
    ~Guard() { s.unlock(); }
};
