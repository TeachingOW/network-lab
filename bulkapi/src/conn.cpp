#include "conn.h"

#include <unistd.h>

#include <algorithm>
#include <cstdlib>
#include <vector>

static thread_local std::vector<Conn*> t_pool;

Conn* conn_new(int fd, bool own_buffer) {
    Conn* c;
    if (!t_pool.empty()) { c = t_pool.back(); t_pool.pop_back(); }
    else c = new Conn();
    c->fd = fd;
    c->in_len = c->need_total = c->out_off = 0;
    c->want_close = c->sent100 = c->ep_out = false;
    c->recv_armed = c->recv_cancel = c->sending = c->closing = c->peer_eof = false;
    c->out.clear();
    if (own_buffer && !c->in) { c->in = (char*)malloc(kInitBuf); c->in_cap = kInitBuf; }
    return c;
}

void conn_free(Conn* c) {
    if (c->fd >= 0) close(c->fd);
    c->fd = -1;
    if (c->in_cap > kBigBuf) { free(c->in); c->in = nullptr; c->in_cap = 0; }
    if (c->out.capacity() > (1u << 20)) std::string().swap(c->out);
    t_pool.push_back(c);
}

void ensure_room(Conn& c) {
    size_t want = std::max(c.need_total, c.in_len + 4096);
    if (want > c.in_cap) {
        size_t ncap = (c.need_total >= c.in_len + 4096) ? c.need_total
                                                       : std::max(want, c.in_cap * 2);
        c.in = (char*)realloc(c.in, ncap);
        c.in_cap = ncap;
    }
}
