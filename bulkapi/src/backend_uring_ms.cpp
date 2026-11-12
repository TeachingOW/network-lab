// io_uring backend with multishot receive (`--backend uring-ms`), on liburing.
//
// One io_uring_prep_recv_multishot() per connection stays armed and posts a
// completion for every chunk of data, each in a buffer the kernel picks from a
// per-worker provided-buffer ring. Needs Linux 6.1+ and liburing 2.3+; there is
// no fallback: if the ring cannot be set up the server exits.
//
// Differences from backend_uring.cpp (single-shot receive):
//   - no RECV re-submission per request: the receive is armed once
//   - no buffer per connection: a request is parsed in place in the shared 16 KB
//     receive buffer, and an output buffer is borrowed from a per-worker pool
//     only while a reply exists. A private input buffer is allocated only while
//     a request is incomplete or input arrives during a send, and freed after.
//   - a receive stays armed while a send is in flight, so a connection can
//     have two operations outstanding; it is only freed once both have ended

#include <liburing.h>
#include <sys/mman.h>
#include <sys/socket.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "backend.h"
#include "http.h"
#include "net.h"

namespace {

constexpr unsigned kBufs = 1024;         // per worker; must be a power of two
constexpr unsigned kBufSize = 16 * 1024;
constexpr int kBufGroup = 0;
constexpr size_t kBacklogCap = 1 << 20;  // unprocessed input allowed while a send is in flight
constexpr size_t kSso = 15;              // capacity of an empty std::string (no heap buffer)

// user_data = Conn* | op
enum : u64 { OP_ACCEPT = 0, OP_RECV = 1, OP_SEND = 2, OP_CANCEL = 3, OP_MASK = 7 };

struct Loop {
    io_uring ring{};
    io_uring_buf_ring* br = nullptr;
    char* bufs = nullptr;
    Worker& w;
    std::vector<Conn*> rearm;  // connections whose receive ended and should be armed again
    std::vector<std::string> outs;  // spare output buffers, lent to a connection while it has a reply
    explicit Loop(Worker& wk) : w(wk) {}

    bool init() {
        io_uring_params p{};
        p.flags = IORING_SETUP_CQSIZE | IORING_SETUP_SINGLE_ISSUER | IORING_SETUP_DEFER_TASKRUN;
        p.cq_entries = 32768;
        int rc = io_uring_queue_init_params(4096, &ring, &p);
        if (rc < 0) { fprintf(stderr, "worker %d: io_uring_queue_init_params: %s\n", w.id, strerror(-rc)); return false; }
        br = io_uring_setup_buf_ring(&ring, kBufs, kBufGroup, 0, &rc);
        if (!br) { fprintf(stderr, "worker %d: io_uring_setup_buf_ring: %s\n", w.id, strerror(-rc)); return false; }
        void* m = mmap(nullptr, (size_t)kBufs * kBufSize, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_POPULATE, -1, 0);
        if (m == MAP_FAILED) { perror("mmap receive buffers"); return false; }
        bufs = (char*)m;
        for (unsigned i = 0; i < kBufs; i++)
            io_uring_buf_ring_add(br, buf_ptr(i), kBufSize, (unsigned short)i, io_uring_buf_ring_mask(kBufs), (int)i);
        io_uring_buf_ring_advance(br, (int)kBufs);
        return true;
    }
    char* buf_ptr(unsigned bid) { return bufs + (size_t)bid * kBufSize; }
    // Hand a buffer back to the kernel.
    void buf_return(unsigned bid) {
        io_uring_buf_ring_add(br, buf_ptr(bid), kBufSize, (unsigned short)bid, io_uring_buf_ring_mask(kBufs), 0);
        io_uring_buf_ring_advance(br, 1);
    }

    io_uring_sqe* sqe() {
        io_uring_sqe* e = io_uring_get_sqe(&ring);
        if (!e) {  // submission queue full: flush it
            io_uring_submit(&ring);
            e = io_uring_get_sqe(&ring);
        }
        return e;
    }

    void prep_recv_multishot(Conn* c) {
        io_uring_sqe* e = sqe();
        io_uring_prep_recv_multishot(e, c->fd, nullptr, 0, 0);
        e->flags |= IOSQE_BUFFER_SELECT;
        e->buf_group = kBufGroup;
        io_uring_sqe_set_data64(e, (u64)c | OP_RECV);
        c->recv_armed = true;
        c->recv_cancel = false;
    }
    void prep_send(Conn* c) {
        io_uring_sqe* e = sqe();
        io_uring_prep_send(e, c->fd, c->out.data() + c->out_off, c->out.size() - c->out_off, MSG_NOSIGNAL);
        io_uring_sqe_set_data64(e, (u64)c | OP_SEND);
        c->sending = true;
    }
    void prep_cancel_recv(Conn* c) {
        io_uring_sqe* e = sqe();
        io_uring_prep_cancel64(e, (u64)c | OP_RECV, 0);
        io_uring_sqe_set_data64(e, OP_CANCEL);
        c->recv_cancel = true;
    }
    void prep_accept(int lfd) {
        io_uring_sqe* e = sqe();
        io_uring_prep_multishot_accept(e, lfd, nullptr, nullptr, SOCK_CLOEXEC);
        io_uring_sqe_set_data64(e, OP_ACCEPT);
    }

    // A connection is freed only when neither a receive nor a send can still complete on it.
    void maybe_free(Conn* c) {
        if (!c->closing || c->recv_armed || c->sending) return;
        drop_in(c);
        give_out(c);
        conn_free(c);
    }
    void start_close(Conn* c) {
        if (c->closing) return;
        c->closing = true;
        if (c->recv_armed || c->sending) shutdown(c->fd, SHUT_RDWR);  // ends the armed receive / the send
        maybe_free(c);
    }

    // --- buffers. A connection owns none while idle: input is parsed in the shared receive
    // buffer, and an output buffer is borrowed from the worker's pool only while a reply exists.

    static void drop_in(Conn* c) {
        free(c->in);
        c->in = nullptr;
        c->in_cap = c->in_len = 0;
    }
    // Keeps bytes that cannot be handled yet (a partial request, or input that arrived mid-send).
    static void stash(Conn* c, const char* p, size_t n) {
        size_t want = c->in_len + n;
        if (want > c->in_cap) {
            size_t ncap = std::max(want, c->in_cap * 2);  // doubling: an announced body is not reserved up front
            if (c->need_total >= want) ncap = std::min(ncap, c->need_total);
            c->in = (char*)realloc(c->in, ncap);
            c->in_cap = ncap;
        }
        memcpy(c->in + c->in_len, p, n);
        c->in_len += n;
    }
    void take_out(Conn* c) {
        if (c->out.capacity() > kSso || outs.empty()) return;
        c->out.swap(outs.back());
        outs.pop_back();
    }
    void give_out(Conn* c) {
        if (c->out.capacity() <= kSso) return;
        c->out.clear();
        if (c->out.capacity() <= (1u << 20) && outs.size() < 4096) outs.push_back(std::move(c->out));
        c->out = std::string();
    }

    // Restart the timeout that matches the connection's state (after every event on it).
    static void touch(Conn* c) {
        if (c->fd >= 0 && !c->closing) timer_update(*c, c->sending);
    }

    // A deadline passed. Half a request held: answer 408 and close after it is sent. Otherwise
    // (idle, or a send not progressing) close; shutdown() makes the operations in flight end.
    void expire(Conn* c) {
        if (c->closing) return;
        if ((c->expired == T_HEADER || c->expired == T_BODY) && !c->sending) {
            drop_in(c);
            c->need_total = 0;
            take_out(c);
            append_timeout_response(c->out);
            c->want_close = true;
            if (c->recv_armed && !c->recv_cancel) prep_cancel_recv(c);
            prep_send(c);
            touch(c);
            return;
        }
        start_close(c);
    }

    // After process(): send what it produced, or hand the output buffer back.
    void after_process(Conn* c) {
        if (!c->out.empty()) { prep_send(c); return; }
        give_out(c);
        if (c->want_close) start_close(c);
    }

    // Handle the requests held in the connection's private buffer.
    void run(Conn* c) {
        if (c->need_total && c->in_len < c->need_total) return;  // a large body is still arriving
        take_out(c);
        process(*c, w);
        if (c->in_len == 0) drop_in(c);
        after_process(c);
    }

    // New bytes from the shared receive buffer p[0..n), which goes back to the kernel on return.
    void feed(Conn* c, char* p, size_t n) {
        if (c->in_len || c->sending) {  // already holding input, or a reply is in flight: keep a copy
            stash(c, p, n);
            if (!c->sending) run(c);
            else if (c->in_len > kBacklogCap && c->recv_armed && !c->recv_cancel) prep_cancel_recv(c);  // backpressure
            return;
        }
        // The common case: parse in place, no copy and no per-connection buffer.
        c->in = p;
        c->in_cap = c->in_len = n;
        take_out(c);
        process(*c, w);  // leaves any incomplete request at the front of p
        size_t left = c->in_len;
        c->in = nullptr;
        c->in_cap = c->in_len = 0;
        if (left) stash(c, p, left);
        after_process(c);
    }

    void on_recv(Conn* c, int res, unsigned flags) {
        bool has_buf = flags & IORING_CQE_F_BUFFER;
        unsigned bid = flags >> IORING_CQE_BUFFER_SHIFT;
        if (!(flags & IORING_CQE_F_MORE)) c->recv_armed = c->recv_cancel = false;  // the multishot ended
        if (res > 0 && has_buf && !c->closing && !c->want_close) feed(c, buf_ptr(bid), (size_t)res);
        if (has_buf) buf_return(bid);
        if (c->fd < 0) return;  // feed() closed and freed it
        if (c->closing) { maybe_free(c); return; }
        if (res > 0) {
            if (!c->recv_armed) rearm.push_back(c);
            touch(c);
        } else if (res == -ENOBUFS || res == -EINTR || res == -EAGAIN) {
            rearm.push_back(c);  // buffer ring ran dry: arm again after this batch has returned its buffers
        } else if (res == -ECANCELED) {
            if (!c->sending) rearm.push_back(c);  // otherwise the send completion arms it again
        } else if (res == -EINVAL) {
            fprintf(stderr, "worker %d: multishot receive is not supported by this kernel (needs Linux 6.0+)\n", w.id);
            exit(1);
        } else if (c->sending) {
            c->peer_eof = true;  // EOF or error: finish the reply in flight first
        } else {
            start_close(c);
        }
    }

    void on_send(Conn* c, int res) {
        c->sending = false;
        if (c->closing) { maybe_free(c); return; }
        if (res < 0) { start_close(c); return; }
        c->out_off += res;
        if (c->out_off < c->out.size()) { prep_send(c); touch(c); return; }
        c->out.clear();
        c->out_off = 0;
        if (c->want_close) { give_out(c); start_close(c); return; }
        if (c->in_len) run(c);  // requests that arrived while the send was in flight
        else give_out(c);
        if (c->fd < 0 || c->closing) return;
        if (c->peer_eof) { if (!c->sending) start_close(c); return; }
        if (!c->recv_armed) rearm.push_back(c);
        touch(c);
    }
};

}  // namespace

bool run_uring_ms(Worker& w, int lfd) {
    Loop L(w);
    if (!L.init()) return false;
    long batch_us = g_batch_wait_us;  // --batch-wait-us, see conn.h
    fprintf(stderr, "worker %d: io_uring multishot recv (liburing %d.%d, %u x %u KB shared buffers, batch wait %ld us)\n",
            w.id, io_uring_major_version(), io_uring_minor_version(), kBufs, kBufSize / 1024, batch_us);
    L.prep_accept(lfd);
    unsigned last = 0;
    timer_clock();
    for (;;) {
        int rc;
        io_uring_cqe* cqe;
        int tmo = timer_wait_ms();
        if (batch_us > 0 && last >= kBusy && tmo != 0) {
            long us = tmo < 0 ? batch_us : std::min<long>(batch_us, tmo * 1000L);  // never past a deadline
            __kernel_timespec ts{us / 1000000, (us % 1000000) * 1000};
            rc = io_uring_submit_and_wait_timeout(&L.ring, &cqe, std::min(last / 2, 64u), &ts, nullptr);
        } else if (tmo >= 0) {
            __kernel_timespec ts{tmo / 1000, (long long)(tmo % 1000) * 1000000};
            rc = io_uring_submit_and_wait_timeout(&L.ring, &cqe, 1, &ts, nullptr);
        } else {
            rc = io_uring_submit_and_wait(&L.ring, 1);
        }
        timer_clock();
        if (rc < 0 && rc != -EINTR && rc != -EAGAIN && rc != -EBUSY && rc != -ETIME) {
            fprintf(stderr, "io_uring_submit_and_wait: %s\n", strerror(-rc));
            return true;
        }
        unsigned head, seen = 0;
        io_uring_for_each_cqe(&L.ring, head, cqe) {
            seen++;
            int res = cqe->res;
            unsigned flags = cqe->flags;
            u64 op = cqe->user_data & OP_MASK;
            Conn* c = (Conn*)(cqe->user_data & ~OP_MASK);
            if (op == OP_ACCEPT) {
                if (res >= 0) {
                    nodelay(res);
                    Conn* nc = conn_new(res, false);  // no private input buffer
                    w.st->conns.store(w.st->conns.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
                    L.prep_recv_multishot(nc);
                    timer_update(*nc, false);
                }
                if (!(flags & IORING_CQE_F_MORE)) L.prep_accept(lfd);
            } else if (op == OP_RECV) {
                L.on_recv(c, res, flags);
            } else if (op == OP_SEND) {
                L.on_send(c, res);
            }  // OP_CANCEL: nothing to do; the cancelled receive posts its own completion
        }
        io_uring_cq_advance(&L.ring, seen);
        last = seen;
        for (Conn* c : L.rearm)
            if (c->fd >= 0 && !c->closing && !c->want_close && !c->recv_armed && !c->peer_eof) L.prep_recv_multishot(c);
        L.rearm.clear();
        while (Conn* c = timer_pop_expired()) L.expire(c);
    }
}
