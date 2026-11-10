// bulkapi: spec-driven bulk REST server. io_uring backend (raw syscalls, no
// liburing needed) with an epoll fallback. Thread-per-core, SO_REUSEPORT,
// pipelining-aware HTTP/1.1 parser, sharded in-memory store.
//
// Routes come from api.spec via tools/gen_routes.py -> build/routes_gen.h.
//
//   main.cpp            CLI, worker threads, backend selection
//   backend_uring.cpp   io_uring event loop
//   backend_epoll.cpp   epoll event loop
//   net.{h,cpp}         listener / socket options
//   http.{h,cpp}        request framing, routing, response serialisation
//   conn.{h,cpp}        Worker, Conn, buffer management
//   handlers.cpp        the h_<name> route handlers
//   json.{h,cpp}        item / NDJSON parsing and escaping
//   store.{h,cpp}       sharded in-memory store
//   types.h             Req, Res, Stats

#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

#include "backend.h"
#include "net.h"

Stats g_stats[kMaxWorkers];
int g_nworkers = 1;

enum Backend { B_AUTO, B_URING, B_EPOLL };

static void worker_main(int id, int port, Backend be, bool pin) {
    Worker w;
    w.id = id;
    w.port = port;
    w.st = &g_stats[id];
    if (pin) {
        cpu_set_t set;
        CPU_ZERO(&set);
        CPU_SET(id % std::thread::hardware_concurrency(), &set);
        pthread_setaffinity_np(pthread_self(), sizeof set, &set);
    }
    int lfd = make_listener(port);
    if (lfd < 0) { perror("listen"); exit(1); }
    if (be != B_EPOLL) {
        if (run_uring(w, lfd)) return;
        if (be == B_URING) { fprintf(stderr, "worker %d: io_uring unavailable\n", id); exit(1); }
        fprintf(stderr, "worker %d: io_uring unavailable, using epoll\n", id);
    } else {
        fprintf(stderr, "worker %d: epoll\n", id);
    }
    run_epoll(w, lfd);
}

static void on_signal(int) { _exit(0); }

int main(int argc, char** argv) {
    int port = 8080, threads = (int)std::thread::hardware_concurrency();
    Backend be = B_AUTO;
    bool pin = false;
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        auto next = [&]() -> const char* { if (i + 1 >= argc) { fprintf(stderr, "missing value for %s\n", a.c_str()); exit(2); } return argv[++i]; };
        if (a == "-p" || a == "--port") port = atoi(next());
        else if (a == "-t" || a == "--threads") threads = atoi(next());
        else if (a == "--pin") pin = true;
        else if (a == "--backend") {
            std::string b = next();
            be = b == "uring" ? B_URING : b == "epoll" ? B_EPOLL : B_AUTO;
        } else {
            fprintf(stderr, "usage: %s [-p port] [-t threads] [--backend auto|uring|epoll] [--pin]\n", argv[0]);
            return 2;
        }
    }
    threads = std::clamp(threads, 1, kMaxWorkers);
    g_nworkers = threads;
    signal(SIGPIPE, SIG_IGN);
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    fprintf(stderr, "bulkapi listening on :%d with %d workers\n", port, threads);
    std::vector<std::thread> ts;
    for (int i = 0; i < threads; i++) ts.emplace_back(worker_main, i, port, be, pin);
    for (auto& t : ts) t.join();
}
