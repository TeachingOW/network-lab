// bulkapi: spec-driven bulk REST server on io_uring (or epoll when asked for). No
// fallback: a backend that cannot start stops the server. Thread-per-core, SO_REUSEPORT,
// pipelining-aware HTTP/1.1 parser, sharded in-memory store.
//
// Routes come from api.spec via tools/gen_routes.py -> build/routes_gen.h.
//
//   main.cpp            CLI, worker threads, backend selection
//   backend_uring.cpp   io_uring event loop (single-shot receive)
//   backend_uring_ms.cpp  io_uring event loop with multishot receive
//   uring.h             raw-syscall io_uring wrapper shared by both
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

enum Backend { B_URING, B_URING_MS, B_EPOLL };

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
    switch (be) {
    case B_URING: if (run_uring(w, lfd)) return; break;
    case B_URING_MS: if (run_uring_ms(w, lfd)) return; break;
    case B_EPOLL: fprintf(stderr, "worker %d: epoll\n", id); run_epoll(w, lfd); return;
    }
    fprintf(stderr, "worker %d: the io_uring backend could not start (needs Linux 6.1+)\n", id);
    exit(1);
}

static void on_signal(int) { _exit(0); }

static uint64_t secs_to_ms(const std::string& flag, const char* v) {
    char* end;
    double s = strtod(v, &end);
    if (*end || s < 0) { fprintf(stderr, "%s: expected seconds, got '%s'\n", flag.c_str(), v); exit(2); }
    return (uint64_t)(s * 1000 + 0.5);
}

int main(int argc, char** argv) {
    int port = 8080, threads = (int)std::thread::hardware_concurrency();
    Backend be = B_URING;
    bool pin = false;
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        auto next = [&]() -> const char* { if (i + 1 >= argc) { fprintf(stderr, "missing value for %s\n", a.c_str()); exit(2); } return argv[++i]; };
        if (a == "-p" || a == "--port") port = atoi(next());
        else if (a == "-t" || a == "--threads") threads = atoi(next());
        else if (a == "--pin") pin = true;
        else if (a == "--batch-wait-us") g_batch_wait_us = atol(next());
        else if (a == "--idle-timeout") g_timeout_ms[T_IDLE] = secs_to_ms(a, next());
        else if (a == "--header-timeout") g_timeout_ms[T_HEADER] = secs_to_ms(a, next());
        else if (a == "--body-timeout") g_timeout_ms[T_BODY] = secs_to_ms(a, next());
        else if (a == "--write-timeout") g_timeout_ms[T_WRITE] = secs_to_ms(a, next());
        else if (a == "--backend") {
            std::string b = next();
            if (b == "uring") be = B_URING;
            else if (b == "uring-ms") be = B_URING_MS;
            else if (b == "epoll") be = B_EPOLL;
            else { fprintf(stderr, "--backend: expected uring, uring-ms or epoll, got '%s'\n", b.c_str()); return 2; }
        } else {
            fprintf(stderr,
                    "usage: %s [-p port] [-t threads] [--backend uring|uring-ms|epoll] [--pin]\n"
                    "          [--batch-wait-us N] [--idle-timeout S] [--header-timeout S] [--body-timeout S] [--write-timeout S]\n"
                    "timeouts are in seconds (fractions allowed), 0 disables one; defaults 60, 10, 30, 30\n"
                    "batch wait: microseconds to wait for more completions when busy, 0 = off; default 200\n",
                    argv[0]);
            return 2;
        }
    }
    threads = std::clamp(threads, 1, kMaxWorkers);
    g_nworkers = threads;
    signal(SIGPIPE, SIG_IGN);
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    fprintf(stderr, "bulkapi listening on :%d with %d workers (timeouts ms: idle %llu, header %llu, body %llu, write %llu)\n",
            port, threads, (unsigned long long)g_timeout_ms[T_IDLE], (unsigned long long)g_timeout_ms[T_HEADER],
            (unsigned long long)g_timeout_ms[T_BODY], (unsigned long long)g_timeout_ms[T_WRITE]);
    std::vector<std::thread> ts;
    for (int i = 0; i < threads; i++) ts.emplace_back(worker_main, i, port, be, pin);
    for (auto& t : ts) t.join();
}
