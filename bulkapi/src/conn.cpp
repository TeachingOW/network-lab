#include "conn.h"

#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <cstdlib>
#include <vector>

long g_batch_wait_us = 200;
uint64_t g_timeout_ms[T_KINDS] = {0, 60000, 10000, 30000, 30000};

static thread_local std::vector<Conn*> t_pool;

Conn* conn_new(int fd, bool own_buffer) {
    Conn* c;
    if (!t_pool.empty()) { c = t_pool.back(); t_pool.pop_back(); }
    else c = new Conn();
    c->fd = fd;
    c->in_len = c->need_total = c->out_off = 0;
    c->want_close = c->sent100 = c->ep_out = c->req_done = false;
    c->recv_armed = c->recv_cancel = c->sending = c->closing = c->peer_eof = false;
    c->tkind = c->expired = T_NONE;
    c->out.clear();
    if (own_buffer && !c->in) { c->in = (char*)malloc(kInitBuf); c->in_cap = kInitBuf; }
    return c;
}

void conn_free(Conn* c) {
    timer_stop(*c);
    if (c->fd >= 0) close(c->fd);
    c->fd = -1;
    if (c->in_cap > kBigBuf) { free(c->in); c->in = nullptr; c->in_cap = 0; }
    if (c->out.capacity() > (1u << 20)) std::string().swap(c->out);
    t_pool.push_back(c);
}

// Room for the next read. The buffer grows by doubling, toward the request's total size when the
// header has announced it, but never straight to it: a client that announces a 64 MB body and then
// sends nothing must not get 64 MB reserved for it.
void ensure_room(Conn& c) {
    size_t need = c.in_len + 4096;
    if (need <= c.in_cap) return;
    size_t ncap = std::max(need, c.in_cap * 2);
    if (c.need_total >= need) ncap = std::min(ncap, c.need_total);
    c.in = (char*)realloc(c.in, ncap);
    c.in_cap = ncap;
}

// ---------------- timeouts ----------------

namespace {
struct TList { Conn *head = nullptr, *tail = nullptr; };
thread_local TList t_lists[T_KINDS];
thread_local uint64_t t_now;

void unlink(Conn& c) {
    TList& l = t_lists[c.tkind];
    if (c.tprev) c.tprev->tnext = c.tnext; else l.head = c.tnext;
    if (c.tnext) c.tnext->tprev = c.tprev; else l.tail = c.tprev;
    c.tprev = c.tnext = nullptr;
}
bool linked(const Conn& c) { return c.tkind != T_NONE && (c.tprev || t_lists[c.tkind].head == &c); }
}  // namespace

uint64_t timer_clock() {
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC_COARSE, &ts);
    return t_now = (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}
uint64_t timer_now() { return t_now; }

void timer_stop(Conn& c) {
    if (linked(c)) unlink(c);
    c.tkind = T_NONE;
}

void timer_update(Conn& c, bool out_pending) {
    TKind k = out_pending ? T_WRITE : c.in_len == 0 ? T_IDLE : c.need_total ? T_BODY : T_HEADER;
    // The header timeout runs from the first byte of a request and is not extended by more bytes
    // (a client sending one header byte at a time must still be cut off), unless a request was
    // completed in between: then the bytes held are the start of the next one.
    if (k == T_HEADER && c.tkind == T_HEADER && !c.req_done) return;
    c.req_done = false;
    if (linked(c)) unlink(c);
    c.tkind = k;
    uint64_t ms = g_timeout_ms[k];
    if (!ms) return;  // this kind is not timed; tkind still records the state
    c.deadline = t_now + ms;
    TList& l = t_lists[k];
    c.tprev = l.tail;
    c.tnext = nullptr;
    if (l.tail) l.tail->tnext = &c; else l.head = &c;
    l.tail = &c;
}

Conn* timer_pop_expired() {
    for (int k = T_IDLE; k < T_KINDS; k++) {
        Conn* c = t_lists[k].head;
        if (c && c->deadline <= t_now) {
            unlink(*c);
            c->expired = (TKind)k;
            c->tkind = T_NONE;
            return c;
        }
    }
    return nullptr;
}

int timer_wait_ms() {
    uint64_t best = UINT64_MAX;
    for (int k = T_IDLE; k < T_KINDS; k++)
        if (t_lists[k].head) best = std::min(best, t_lists[k].head->deadline);
    if (best == UINT64_MAX) return -1;
    // +5: the coarse clock ticks every few ms; waking a little late avoids waking twice
    return best <= t_now ? 0 : (int)std::min<uint64_t>(best - t_now + 5, 1u << 30);
}
