// Shared request/response types and per-worker counters.
#pragma once

#include <atomic>
#include <cstdint>
#include <string>
#include <string_view>

#include "routes_gen.h"  // Method, Params, route tables (generated from api.spec)

using u64 = uint64_t;

struct Req {
    Method method;
    std::string_view path, query, body;
    Params params;
};
struct Res {
    int status = 200;
    const char* ctype = "application/json";
    std::string body;
};

// Appends {"error":"msg"} and sets the status.
inline void err(Res& r, int status, const char* msg) {
    r.status = status;
    r.body += "{\"error\":\"";
    r.body += msg;
    r.body += "\"}";
}

inline constexpr int kMaxWorkers = 256;

struct alignas(64) Stats {
    std::atomic<u64> req{0}, conns{0};
};
extern Stats g_stats[kMaxWorkers];  // defined in main.cpp
extern int g_nworkers;
