// io_uring backend on raw syscalls (no liburing): one ring per worker,
// multishot accept, one recv or send in flight per connection.
//
// Timeouts: each loop turn waits at most until the earliest connection deadline. An expired
// connection always has exactly one operation in flight; shutdown() makes that operation complete,
// and its completion then frees the connection (after a 408 for a half-received request), so a
// Conn is never freed while the kernel may still post a completion for it.

#include <sys/socket.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>

#include "backend.h"
#include "http.h"
#include "net.h"
#include "uring.h"

namespace {

// user_data = Conn* | op (Conn is at least 8-byte aligned, so the low 3 bits are free)
enum : u64 { OP_ACCEPT = 0, OP_RECV = 1, OP_SEND = 2, OP_MASK = 7 };

inline void sq_recv(Uring& r, Conn* c) {
    ensure_room(*c);
    io_uring_sqe* e = r.get_sqe();
    e->opcode = IORING_OP_RECV;
    e->fd = c->fd;
    e->addr = (u64)(c->in + c->in_len);
    e->len = (unsigned)(c->in_cap - c->in_len);
    // A receive is only armed once the previous input is used up, so its data has almost never
    // arrived yet: POLL_FIRST skips the attempt that would fail and goes straight to waiting.
    e->ioprio = IORING_RECVSEND_POLL_FIRST;
    e->user_data = (u64)c | OP_RECV;
}
inline void sq_send(Uring& r, Conn* c) {
    io_uring_sqe* e = r.get_sqe();
    e->opcode = IORING_OP_SEND;
    e->fd = c->fd;
    e->addr = (u64)(c->out.data() + c->out_off);
    e->len = (unsigned)(c->out.size() - c->out_off);
    e->msg_flags = MSG_NOSIGNAL;
    e->user_data = (u64)c | OP_SEND;
}
inline void sq_accept(Uring& r, int lfd) {
    io_uring_sqe* e = r.get_sqe();
    e->opcode = IORING_OP_ACCEPT;
    e->fd = lfd;
    e->accept_flags = SOCK_CLOEXEC;
    e->ioprio = IORING_ACCEPT_MULTISHOT;
    e->user_data = OP_ACCEPT;
}

// After a recv/send finished (or a new conn arrived): decide the next op.
void next_op(Uring& r, Conn* c) {
    if (c->out_off < c->out.size()) {
        timer_update(*c, true);
        sq_send(r, c);
        return;
    }
    c->out.clear();
    c->out_off = 0;
    if (c->out.capacity() > (1u << 20)) std::string().swap(c->out);
    if (c->want_close) { conn_free(c); return; }
    timer_update(*c, false);
    sq_recv(r, c);
}

// A deadline passed. The connection has one operation in flight; make it complete.
void expire(Conn* c) {
    // Half a request held: shut only the read side, so the receive ends and a 408 can still be
    // sent. Otherwise (idle, or a send not progressing) shut both, and the completion frees it.
    bool partial = c->expired == T_HEADER || c->expired == T_BODY;
    shutdown(c->fd, partial ? SHUT_RD : SHUT_RDWR);
}

}  // namespace

bool run_uring(Worker& w, int lfd) {
    Uring r;
    if (!r.init(4096, 32768)) { fprintf(stderr, "worker %d: io_uring setup: %s\n", w.id, r.error); return false; }
    fprintf(stderr, "worker %d: io_uring single-shot recv (batch wait %ld us)\n", w.id, g_batch_wait_us);
    sq_accept(r, lfd);
    timer_clock();
    unsigned last = 0;  // completions handled in the previous turn
    for (;;) {
        int tmo = timer_wait_ms();
        long long wait_us = tmo < 0 ? -1 : (long long)tmo * 1000;
        unsigned want = 1;
        if (g_batch_wait_us > 0 && last >= kBusy) {
            wait_us = wait_us < 0 ? g_batch_wait_us : std::min<long long>(wait_us, g_batch_wait_us);
            want = std::min(last / 2, 64u);
        }
        if (r.enter(true, wait_us, want) < 0) { perror("io_uring_enter"); return true; }
        timer_clock();
        unsigned head = *r.cq_head;
        last = __atomic_load_n(r.cq_tail, __ATOMIC_ACQUIRE) - head;
        unsigned tail = __atomic_load_n(r.cq_tail, __ATOMIC_ACQUIRE);
        while (head != tail) {
            io_uring_cqe cqe = r.cqes[head & *r.cq_mask];
            head++;
            int res = cqe.res;
            u64 op = cqe.user_data & OP_MASK;
            Conn* c = (Conn*)(cqe.user_data & ~OP_MASK);
            if (op == OP_ACCEPT) {
                if (res >= 0) {
                    nodelay(res);
                    Conn* nc = conn_new(res);
                    w.st->conns.store(w.st->conns.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
                    timer_update(*nc, false);
                    sq_recv(r, nc);
                } else if (res == -EINVAL) {
                    fprintf(stderr, "worker %d: multishot accept is not supported by this kernel (needs Linux 5.19+)\n", w.id);
                    exit(1);
                }
                if (!(cqe.flags & IORING_CQE_F_MORE)) sq_accept(r, lfd);
            } else if (op == OP_RECV) {
                if (res == -EINVAL) {
                    fprintf(stderr, "worker %d: IORING_RECVSEND_POLL_FIRST is not supported by this kernel (needs Linux 5.19+)\n", w.id);
                    exit(1);
                }
                if ((res == -EINTR || res == -EAGAIN) && !c->expired) { sq_recv(r, c); continue; }
                if (res <= 0 || c->expired == T_IDLE) {
                    if ((c->expired == T_HEADER || c->expired == T_BODY) && c->in_len) {
                        c->in_len = 0;  // timed out half way through a request: answer 408, then close
                        append_timeout_response(c->out);
                        c->want_close = true;
                        next_op(r, c);
                    } else {
                        conn_free(c);
                    }
                    continue;
                }
                c->in_len += res;
                process(*c, w);
                next_op(r, c);
            } else {  // OP_SEND
                if (res < 0) { conn_free(c); continue; }
                c->out_off += res;
                next_op(r, c);
            }
        }
        __atomic_store_n(r.cq_head, head, __ATOMIC_RELEASE);
        while (Conn* c = timer_pop_expired()) expire(c);
    }
}
