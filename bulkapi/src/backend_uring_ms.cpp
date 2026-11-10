// io_uring backend with multishot receive (`--backend uring-ms`), on liburing.
//
// One io_uring_prep_recv_multishot() per connection stays armed and posts a
// completion for every chunk of data, each in a buffer the kernel picks from a
// per-worker provided-buffer ring. Needs Linux 6.1+ and liburing 2.3+; there is
// no fallback: if the ring cannot be set up the server exits.
//
// Differences from backend_uring.cpp (single-shot receive):
//   - no RECV re-submission per request: the receive is armed once
//   - data arrives in shared 16 KB buffers and is copied into the connection's
//     own buffer, so a request costs one extra copy
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

// user_data = Conn* | op
enum : u64 { OP_ACCEPT = 0, OP_RECV = 1, OP_SEND = 2, OP_CANCEL = 3, OP_MASK = 7 };

struct Loop {
    io_uring ring{};
    io_uring_buf_ring* br = nullptr;
    char* bufs = nullptr;
    Worker& w;
    std::vector<Conn*> rearm;  // connections whose receive ended and should be armed again
    explicit Loop(Worker& wk) : w(wk) {}

    bool init() {
        io_uring_params p{};
        p.flags = IORING_SETUP_CQSIZE | IORING_SETUP_SINGLE_ISSUER | IORING_SETUP_DEFER_TASKRUN;
        p.cq_entries = 32768;
        int rc = io_uring_queue_init_params(4096, &ring, &p);
        if (rc < 0) { fprintf(stderr, "worker %d: io_uring_queue_init_params: %s\n", w.id, strerror(-rc)); return false; }
        br = io_uring_setup_buf_ring(&ring, kBufs, kBufGroup, 0, &rc);
        if (!br) { fprintf(stderr, "worker %d: io_uring_setup_buf_ring: %s\n", w.id, strerror(-rc)); return false; }
        void* m = mmap(nullptr, (size_t)kBufs * kBufSize, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
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
        if (c->closing && !c->recv_armed && !c->sending) conn_free(c);
    }
    void start_close(Conn* c) {
        if (c->closing) return;
        c->closing = true;
        if (c->recv_armed || c->sending) shutdown(c->fd, SHUT_RDWR);  // ends the armed receive / the send
        maybe_free(c);
    }

    static void append_in(Conn* c, const char* p, size_t n) {
        size_t want = c->in_len + n;
        if (want > c->in_cap) {
            size_t ncap = std::max({want, c->need_total, c->in_cap * 2});
            c->in = (char*)realloc(c->in, ncap);
            c->in_cap = ncap;
        }
        memcpy(c->in + c->in_len, p, n);
        c->in_len += n;
    }

    // Handle whatever complete requests are buffered; start a send if that produced output.
    void run(Conn* c) {
        if (c->need_total && c->in_len < c->need_total) return;  // a large body is still arriving
        process(*c, w);
        if (!c->out.empty()) prep_send(c);
        else if (c->want_close) start_close(c);
    }

    void on_recv(Conn* c, int res, unsigned flags) {
        bool has_buf = flags & IORING_CQE_F_BUFFER;
        unsigned bid = flags >> IORING_CQE_BUFFER_SHIFT;
        if (res > 0 && has_buf && !c->closing) append_in(c, buf_ptr(bid), (size_t)res);
        if (has_buf) buf_return(bid);
        if (!(flags & IORING_CQE_F_MORE)) c->recv_armed = c->recv_cancel = false;  // the multishot ended
        if (c->closing) { maybe_free(c); return; }
        if (res > 0) {
            if (!c->sending) run(c);
            else if (c->in_len > kBacklogCap && c->recv_armed && !c->recv_cancel) prep_cancel_recv(c);  // backpressure
            if (!c->recv_armed) rearm.push_back(c);
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
        if (c->out_off < c->out.size()) { prep_send(c); return; }
        c->out.clear();
        c->out_off = 0;
        if (c->out.capacity() > (1u << 20)) std::string().swap(c->out);
        if (c->want_close) { start_close(c); return; }
        if (c->in_len) run(c);  // requests that arrived while the send was in flight
        if (c->fd < 0 || c->closing) return;
        if (c->peer_eof) { if (!c->sending) start_close(c); return; }
        if (!c->recv_armed) rearm.push_back(c);
    }
};

}  // namespace

bool run_uring_ms(Worker& w, int lfd) {
    Loop L(w);
    if (!L.init()) return false;
    fprintf(stderr, "worker %d: io_uring multishot recv (liburing %d.%d, %u x %u KB buffers)\n", w.id,
            io_uring_major_version(), io_uring_minor_version(), kBufs, kBufSize / 1024);
    L.prep_accept(lfd);
    for (;;) {
        int rc = io_uring_submit_and_wait(&L.ring, 1);
        if (rc < 0 && rc != -EINTR && rc != -EAGAIN && rc != -EBUSY) {
            fprintf(stderr, "io_uring_submit_and_wait: %s\n", strerror(-rc));
            return true;
        }
        unsigned head, seen = 0;
        io_uring_cqe* cqe;
        io_uring_for_each_cqe(&L.ring, head, cqe) {
            seen++;
            int res = cqe->res;
            unsigned flags = cqe->flags;
            u64 op = cqe->user_data & OP_MASK;
            Conn* c = (Conn*)(cqe->user_data & ~OP_MASK);
            if (op == OP_ACCEPT) {
                if (res >= 0) {
                    nodelay(res);
                    Conn* nc = conn_new(res);
                    w.st->conns.store(w.st->conns.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
                    L.prep_recv_multishot(nc);
                }
                if (!(flags & IORING_CQE_F_MORE)) L.prep_accept(lfd);
            } else if (op == OP_RECV) {
                L.on_recv(c, res, flags);
            } else if (op == OP_SEND) {
                L.on_send(c, res);
            }  // OP_CANCEL: nothing to do; the cancelled receive posts its own completion
        }
        io_uring_cq_advance(&L.ring, seen);
        for (Conn* c : L.rearm)
            if (c->fd >= 0 && !c->closing && !c->recv_armed && !c->peer_eof) L.prep_recv_multishot(c);
        L.rearm.clear();
    }
}
