// io_uring backend on raw syscalls (no liburing): one ring per worker,
// multishot accept, one recv or send in flight per connection.

#include <linux/io_uring.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>

#include "backend.h"
#include "http.h"
#include "net.h"

// Older kernel headers (e.g. linux-libc-dev 5.15) lack these; the values are
// stable kernel ABI. The running kernel decides at runtime whether they work
// (init() and the accept path already fall back on failure).
#ifndef IORING_SETUP_COOP_TASKRUN
#define IORING_SETUP_COOP_TASKRUN (1U << 8)
#endif
#ifndef IORING_SETUP_SINGLE_ISSUER
#define IORING_SETUP_SINGLE_ISSUER (1U << 12)
#endif
#ifndef IORING_SETUP_DEFER_TASKRUN
#define IORING_SETUP_DEFER_TASKRUN (1U << 13)
#endif
#ifndef IORING_ACCEPT_MULTISHOT
#define IORING_ACCEPT_MULTISHOT (1U << 0)
#endif

namespace {

struct Uring {
    int fd = -1;
    unsigned entries = 0, local_tail = 0, pending = 0;
    unsigned *sq_head, *sq_tail, *sq_mask, *sq_array, *cq_head, *cq_tail, *cq_mask;
    io_uring_sqe* sqes;
    io_uring_cqe* cqes;
    const char* mode = "";

    bool init(unsigned ent, unsigned cq_ent) {
        struct Try { unsigned flags; const char* name; };
        Try tries[] = {
            {IORING_SETUP_CQSIZE | IORING_SETUP_SINGLE_ISSUER | IORING_SETUP_DEFER_TASKRUN, "single-issuer+defer-taskrun"},
            {IORING_SETUP_CQSIZE | IORING_SETUP_COOP_TASKRUN, "coop-taskrun"},
            {IORING_SETUP_CQSIZE, "plain"},
        };
        io_uring_params p;
        for (auto& t : tries) {
            memset(&p, 0, sizeof p);
            p.flags = t.flags;
            p.cq_entries = cq_ent;
            fd = (int)syscall(__NR_io_uring_setup, ent, &p);
            if (fd >= 0) { mode = t.name; break; }
        }
        if (fd < 0) return false;
        size_t sq_sz = p.sq_off.array + p.sq_entries * sizeof(unsigned);
        size_t cq_sz = p.cq_off.cqes + p.cq_entries * sizeof(io_uring_cqe);
        void* sq_ptr; void* cq_ptr;
        if (p.features & IORING_FEAT_SINGLE_MMAP) {
            sq_sz = cq_sz = std::max(sq_sz, cq_sz);
            sq_ptr = cq_ptr = mmap(nullptr, sq_sz, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE, fd, IORING_OFF_SQ_RING);
        } else {
            sq_ptr = mmap(nullptr, sq_sz, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE, fd, IORING_OFF_SQ_RING);
            cq_ptr = mmap(nullptr, cq_sz, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE, fd, IORING_OFF_CQ_RING);
        }
        sqes = (io_uring_sqe*)mmap(nullptr, p.sq_entries * sizeof(io_uring_sqe), PROT_READ | PROT_WRITE,
                                   MAP_SHARED | MAP_POPULATE, fd, IORING_OFF_SQES);
        if (sq_ptr == MAP_FAILED || cq_ptr == MAP_FAILED || sqes == MAP_FAILED) { close(fd); fd = -1; return false; }
        char* s = (char*)sq_ptr; char* q = (char*)cq_ptr;
        sq_head = (unsigned*)(s + p.sq_off.head);
        sq_tail = (unsigned*)(s + p.sq_off.tail);
        sq_mask = (unsigned*)(s + p.sq_off.ring_mask);
        sq_array = (unsigned*)(s + p.sq_off.array);
        cq_head = (unsigned*)(q + p.cq_off.head);
        cq_tail = (unsigned*)(q + p.cq_off.tail);
        cq_mask = (unsigned*)(q + p.cq_off.ring_mask);
        cqes = (io_uring_cqe*)(q + p.cq_off.cqes);
        entries = p.sq_entries;
        for (unsigned i = 0; i < entries; i++) sq_array[i] = i;
        local_tail = *sq_tail;
        return true;
    }

    // Push queued SQEs to the kernel; optionally wait for >= 1 completion.
    int enter(bool wait) {
        __atomic_store_n(sq_tail, local_tail, __ATOMIC_RELEASE);
        for (;;) {
            unsigned flags = wait ? IORING_ENTER_GETEVENTS : 0;
            long r = syscall(__NR_io_uring_enter, fd, pending, wait ? 1u : 0u, flags, nullptr, 0);
            if (r < 0) {
                if (errno == EINTR) continue;
                if (errno == EAGAIN || errno == EBUSY) return 0;  // CQ pressure: caller drains
                return -1;
            }
            pending -= (unsigned)r;
            if (pending && !wait) continue;
            return 0;
        }
    }

    io_uring_sqe* get_sqe() {
        for (;;) {
            unsigned head = __atomic_load_n(sq_head, __ATOMIC_ACQUIRE);
            if (local_tail - head < entries) break;
            enter(false);  // SQ full: flush
        }
        io_uring_sqe* e = &sqes[local_tail & *sq_mask];
        local_tail++;
        pending++;
        memset(e, 0, sizeof *e);
        return e;
    }
};

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
