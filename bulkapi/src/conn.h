// Per-worker state, per-connection buffers (pooled per thread) and connection timeouts.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#include "types.h"

inline constexpr size_t kInitBuf = 16 * 1024;
inline constexpr size_t kBigBuf = 256 * 1024;  // buffers above this are freed when idle

// Batch wait: when the previous loop turn handled at least kBusy completions, wait up to this many
// microseconds for half as many again (at most 64) before handling them, instead of waking for the
// first one. Fewer system calls per request under load; no effect when lightly loaded. 0 = off.
extern long g_batch_wait_us;
inline constexpr unsigned kBusy = 32;

struct Worker {
    int id = 0, cpu = -1, port = 8080;
    Stats* st = nullptr;
};

// What a connection is waiting for, which decides which timeout applies to it.
//   T_IDLE    keep-alive, nothing buffered               -> closed silently
//   T_HEADER  part of a request header has arrived       -> 408, closed (absolute: counted from the first byte)
//   T_BODY    header complete, body still arriving       -> 408, closed (reset whenever more bytes arrive)
//   T_WRITE   a response is being sent                   -> closed (reset whenever the send makes progress)
enum TKind : uint8_t { T_NONE, T_IDLE, T_HEADER, T_BODY, T_WRITE, T_KINDS };

// Timeout per kind in milliseconds, 0 = disabled. Set once in main() before the workers start.
extern uint64_t g_timeout_ms[T_KINDS];

struct Conn {
    int fd = -1;
    char* in = nullptr;
    size_t in_cap = 0, in_len = 0, need_total = 0;
    std::string out;
    size_t out_off = 0;
    bool want_close = false, sent100 = false, ep_out = false;
    bool req_done = false;      // process() completed at least one request in its last call
    // uring-ms backend only: a receive and a send can be outstanding at the same time
    bool recv_armed = false, recv_cancel = false, sending = false, closing = false, peer_eof = false;
    // timeouts: an intrusive FIFO list per kind, owned by the worker thread
    TKind tkind = T_NONE;       // the kind currently timed (T_NONE: not in a list)
    TKind expired = T_NONE;     // set when a timeout fired and the close is in progress
    uint64_t deadline = 0;      // ms, CLOCK_MONOTONIC_COARSE
    Conn *tprev = nullptr, *tnext = nullptr;
};

// Takes a Conn from the calling thread's pool. own_buffer=false leaves it without an input buffer.
Conn* conn_new(int fd, bool own_buffer = true);
void conn_free(Conn* c);      // closes the fd, stops its timer, returns the Conn to the pool
void ensure_room(Conn& c);    // grows c.in so the next read has space

// --- timeouts (all per calling thread) ---
// Each kind has its own list. Every entry in a list was given the same duration, so appending
// at the tail keeps it ordered by deadline: refreshing is O(1) and expiring looks only at heads.
uint64_t timer_clock();                        // reads the coarse clock once; call once per loop turn
uint64_t timer_now();                          // the value the last timer_clock() read
// Recomputes which timeout applies to c from its state and (re)starts it. Call after every event
// that changed c (data received, send progress, request handled). out_pending: a reply is unsent.
void timer_update(Conn& c, bool out_pending);
void timer_stop(Conn& c);
Conn* timer_pop_expired();                     // next connection whose deadline passed (unlinked), or null
int timer_wait_ms();                           // ms until the earliest deadline, -1 if none
