// Compile against either store.h with -I; allocator counters describe the measured window.
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <new>
#include <stdexcept>
#include <string>
#include <string_view>

static size_t allocations, table_bytes;
[[maybe_unused]] static void* counted_malloc(size_t n) { ++allocations; return std::malloc(n); }
[[maybe_unused]] static void* counted_realloc(void* p, size_t n) { ++allocations; return std::realloc(p, n); }
static void* counted_aligned_alloc(size_t a, size_t n) {
    ++allocations; table_bytes += n; return std::aligned_alloc(a, n);
}
#define malloc counted_malloc
#define realloc counted_realloc
#define aligned_alloc counted_aligned_alloc
#if !defined(__i386__) && !defined(__x86_64__)
// Baseline's x86 spin hint is never used by this single-threaded benchmark.
#define __builtin_ia32_pause() ((void)0)
#endif
#include "store.h"
#undef malloc
#undef realloc
#undef aligned_alloc

int main(int argc, char** argv) {
    if (argc != 2) return 1;
    std::string_view mode = argv[1];
    std::string value(8192, 'x');
    Table t;
    const u64 count = mode == "overwrite" ? 48 : 4096;
    if (mode != "insert") for (u64 k = 0; k < count; k++) t.put(k, "value", 5);
    allocations = table_bytes = 0;
    u64 ops = 0, sum = 0;
    auto start = std::chrono::steady_clock::now();
    if (mode == "growing") {
        for (u64 k = 0; k < 128; k++) {
            t.put(k, "v", 1);
            for (size_t n = 49; n <= value.size(); n++) { t.put(k, value.data(), n); ops++; }
        }
    } else if (mode == "overwrite") {
        // Fresh full tables model a shard reaching its load limit before an update.
        for (u64 round = 0; round < 20000; round++) {
            Table shard;
            for (u64 k = 0; k < count; k++) { shard.put(k, "value", 5); ops++; }
            shard.put(round % count, "new", 3); ops++;
            sum += shard.find(round % count)->len;
        }
    } else if (mode == "update") {
        for (u64 k = 0; k < 4000000; k++) { t.put(k % count, "new", 3); ops++; }
        for (u64 k = 0; k < count; k++) sum += t.find(k)->len;
    } else if (mode == "heap-update") {
        for (u64 k = 0; k < count; k++) t.put(k, value.data(), 128);
        allocations = table_bytes = 0;
        start = std::chrono::steady_clock::now();
        for (u64 k = 0; k < 4000000; k++) { t.put(k % count, value.data(), 128); ops++; }
        for (u64 k = 0; k < count; k++) sum += t.find(k)->len;
    } else if (mode == "lookup") {
        for (u64 k = 0; k < 4000000; k++) { sum += t.find(k % count)->len; ops++; }
    } else if (mode == "insert") {
        for (u64 k = 0; k < 500000; k++) { t.put(k, "value", 5); ops++; }
        sum = t.size();
    } else return 1;
    auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count();
    // Retain the mutated values even in aggressive optimized builds.
    for (u64 k = 0; k < 128; k++) if (const Slot* s = t.find(k)) sum += (unsigned char)s->data()[0];
    std::printf("%.*s %.3f %zu %zu %llu\n", int(mode.size()), mode.data(), double(ns) / ops,
                allocations, table_bytes, (unsigned long long)sum);
}
