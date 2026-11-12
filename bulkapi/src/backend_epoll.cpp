// epoll backend: level-triggered, non-blocking read/send, EPOLLOUT only when a
// send would block.

#include <fcntl.h>
#include <sys/epoll.h>
#include <unistd.h>

#include <cerrno>

#include "backend.h"
#include "http.h"
#include "net.h"

static int flush_out(Conn& c) {  // -1 error, 0 flushed, 1 would block
    while (c.out_off < c.out.size()) {
        ssize_t n = send(c.fd, c.out.data() + c.out_off, c.out.size() - c.out_off, MSG_NOSIGNAL);
        if (n > 0) { c.out_off += n; continue; }
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 && errno == EAGAIN) return 1;
        return -1;
    }
    c.out.clear();
    c.out_off = 0;
    if (c.out.capacity() > (1u << 20)) std::string().swap(c.out);
    return 0;
}

void run_epoll(Worker& w, int lfd) {
    fcntl(lfd, F_SETFL, O_NONBLOCK);
    int ep = epoll_create1(EPOLL_CLOEXEC);
    epoll_event ev{};
    ev.events = EPOLLIN;
    ev.data.ptr = nullptr;
    epoll_ctl(ep, EPOLL_CTL_ADD, lfd, &ev);
    epoll_event evs[256];
    auto drop = [&](Conn* c) { epoll_ctl(ep, EPOLL_CTL_DEL, c->fd, nullptr); conn_free(c); };
    timer_clock();
    for (;;) {
        int n = epoll_wait(ep, evs, 256, timer_wait_ms());
        timer_clock();
        for (int i = 0; i < n; i++) {
            Conn* c = (Conn*)evs[i].data.ptr;
            if (!c) {
                int fd;
                while ((fd = accept4(lfd, nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC)) >= 0) {
                    nodelay(fd);
                    Conn* nc = conn_new(fd);
                    epoll_event e2{};
                    e2.events = EPOLLIN;
                    e2.data.ptr = nc;
                    epoll_ctl(ep, EPOLL_CTL_ADD, fd, &e2);
                    timer_update(*nc, false);
                    w.st->conns.store(w.st->conns.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
                }
                continue;
            }
            uint32_t e = evs[i].events;
            if (e & EPOLLERR) { drop(c); continue; }
            if (c->ep_out) {
                if (e & (EPOLLOUT | EPOLLHUP)) {
                    int f = flush_out(*c);
                    if (f < 0) { drop(c); continue; }
                    if (f == 1) timer_update(*c, true);  // progress (or not): the write timeout restarts on each event
                    if (f == 0) {
                        if (c->want_close) { drop(c); continue; }
                        timer_update(*c, false);
                        c->ep_out = false;
                        epoll_event e2{};
                        e2.events = EPOLLIN;
                        e2.data.ptr = c;
                        epoll_ctl(ep, EPOLL_CTL_MOD, c->fd, &e2);
                    }
                }
                continue;
            }
            if (e & (EPOLLIN | EPOLLHUP)) {
                ensure_room(*c);
                ssize_t r = read(c->fd, c->in + c->in_len, c->in_cap - c->in_len);
                if (r == 0 || (r < 0 && errno != EAGAIN && errno != EINTR)) { drop(c); continue; }
                if (r < 0) continue;
                c->in_len += r;
                process(*c, w);
                if (c->out.empty()) {
                    if (c->want_close) drop(c);
                    else timer_update(*c, false);
                    continue;
                }
                int f = flush_out(*c);
                if (f < 0 || (f == 0 && c->want_close)) { drop(c); continue; }
                timer_update(*c, f == 1);
                if (f == 1) {
                    c->ep_out = true;
                    epoll_event e2{};
                    e2.events = EPOLLOUT;
                    e2.data.ptr = c;
                    epoll_ctl(ep, EPOLL_CTL_MOD, c->fd, &e2);
                }
            }
        }
        // Deadlines: a half-received request gets a best-effort 408, anything else is just closed.
        while (Conn* c = timer_pop_expired()) {
            if ((c->expired == T_HEADER || c->expired == T_BODY) && !c->ep_out) {
                std::string r;
                append_timeout_response(r);
                (void)!send(c->fd, r.data(), r.size(), MSG_NOSIGNAL | MSG_DONTWAIT);
            }
            drop(c);
        }
    }
}
