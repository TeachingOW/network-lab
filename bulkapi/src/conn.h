// Per-worker state and per-connection buffers (pooled per thread).
#pragma once

#include <cstddef>
#include <string>

#include "types.h"

inline constexpr size_t kInitBuf = 16 * 1024;
inline constexpr size_t kBigBuf = 256 * 1024;  // buffers above this are freed when idle

struct Worker {
    int id = 0, cpu = -1, port = 8080;
    Stats* st = nullptr;
};

struct Conn {
    int fd = -1;
    char* in = nullptr;
    size_t in_cap = 0, in_len = 0, need_total = 0;
    std::string out;
    size_t out_off = 0;
    bool want_close = false, sent100 = false, ep_out = false;
    // uring-ms backend only: a receive and a send can be outstanding at the same time
    bool recv_armed = false, recv_cancel = false, sending = false, closing = false, peer_eof = false;
};

// Takes a Conn from the calling thread's pool. own_buffer=false leaves it without an input buffer.
Conn* conn_new(int fd, bool own_buffer = true);
void conn_free(Conn* c);      // closes the fd and returns the Conn to the pool
void ensure_room(Conn& c);    // grows c.in so the next read has space
