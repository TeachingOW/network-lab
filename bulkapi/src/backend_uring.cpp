// io_uring backend on raw syscalls (no liburing): one ring per worker,
// multishot accept, one recv or send in flight per connection.

#include <cstdio>

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
inline void sq_accept(Uring& r, int lfd, bool multishot) {
    io_uring_sqe* e = r.get_sqe();
    e->opcode = IORING_OP_ACCEPT;
    e->fd = lfd;
    e->accept_flags = SOCK_CLOEXEC;
    if (multishot) e->ioprio = IORING_ACCEPT_MULTISHOT;
    e->user_data = OP_ACCEPT;
}

// After a recv/send finished (or a new conn arrived): decide the next op.
void next_op(Uring& r, Conn* c) {
    if (c->out_off < c->out.size()) { sq_send(r, c); return; }
    c->out.clear();
    c->out_off = 0;
    if (c->out.capacity() > (1u << 20)) std::string().swap(c->out);
    if (c->want_close) { conn_free(c); return; }
    sq_recv(r, c);
}

}  // namespace

bool run_uring(Worker& w, int lfd) {
    Uring r;
    if (!r.init(4096, 32768)) return false;
    fprintf(stderr, "worker %d: io_uring (%s)\n", w.id, r.mode);
    bool multishot = true;
    sq_accept(r, lfd, multishot);
    for (;;) {
        if (r.enter(true) < 0) { perror("io_uring_enter"); return true; }
        unsigned head = *r.cq_head;
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
                    sq_recv(r, nc);
                } else if (res == -EINVAL && multishot) {
                    multishot = false;  // old kernel: single-shot accept
                }
                if (!(cqe.flags & IORING_CQE_F_MORE)) sq_accept(r, lfd, multishot);
            } else if (op == OP_RECV) {
                if (res == -EINTR || res == -EAGAIN) { sq_recv(r, c); continue; }
                if (res <= 0) { conn_free(c); continue; }
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
    }
}
