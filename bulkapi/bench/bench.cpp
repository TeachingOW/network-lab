// bench: multi-threaded epoll HTTP/1.1 load generator for bulkapi.
// Keep-alive connections, optional request pipelining, latency histogram.
//
//   bench -h 127.0.0.1 -p 8080 -m get -c 256 -t 2 -d 10 -P 8
//   modes: health | get | put | bulk | bulkget      (-n = items per bulk request)

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

using u64 = uint64_t;
using Clock = std::chrono::steady_clock;
static inline u64 now_ns() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count();
}

struct Opts {
    std::string host = "127.0.0.1", mode = "get";
    int port = 8080, conns = 64, threads = 1, secs = 10, warm = 1, pipeline = 1, batch = 100;
    u64 keyspace = 100000;
    bool preload = true;
};

// ---- log-linear histogram (16 sub-buckets per power of two, ns) ----
struct Hist {
    std::vector<u64> b = std::vector<u64>(48 * 16, 0);
    u64 n = 0, sum = 0, max = 0;
    static int idx(u64 v) {
        if (v < 16) return (int)v;
        int e = 63 - __builtin_clzll(v);
        return (e - 3) * 16 + (int)((v >> (e - 4)) & 15);
    }
    static u64 low(int i) {
        if (i < 16) return i;
        int e = i / 16 + 3, sub = i % 16;
        return (u64)(16 + sub) << (e - 4);
    }
    void add(u64 v) { b[idx(v)]++; n++; sum += v; max = std::max(max, v); }
    void merge(const Hist& o) {
        for (size_t i = 0; i < b.size(); i++) b[i] += o.b[i];
        n += o.n; sum += o.sum; max = std::max(max, o.max);
    }
    u64 pct(double p) const {
        u64 target = (u64)std::ceil(p / 100.0 * n), acc = 0;
        for (size_t i = 0; i < b.size(); i++) { acc += b[i]; if (acc >= target) return low((int)i); }
        return max;
    }
};

static inline u64 xs(u64& s) { s ^= s << 13; s ^= s >> 7; s ^= s << 17; return s; }

static std::string make_req(const char* method, const std::string& path, const std::string& body) {
    std::string r = std::string(method) + " " + path + " HTTP/1.1\r\nHost: bench\r\nContent-Type: application/json\r\n";
    if (!body.empty() || method[0] == 'P') r += "Content-Length: " + std::to_string(body.size()) + "\r\n";
    r += "\r\n";
    r += body;
    return r;
}

static std::string bulk_body(u64& s, u64 keyspace, int n) {
    std::string b;
    for (int i = 0; i < n; i++) {
        b += "{\"id\":" + std::to_string(xs(s) % keyspace) + ",\"v\":\"value-0123456789abcdef\"}\n";
    }
    return b;
}
static std::string ids_body(u64& s, u64 keyspace, int n) {
    std::string b;
    for (int i = 0; i < n; i++) b += std::to_string(xs(s) % keyspace) + "\n";
    return b;
}

static std::vector<std::string> build_pool(const Opts& o, u64 seed) {
    std::vector<std::string> pool;
    u64 s = seed * 0x9E3779B97F4A7C15ull + 1;
    bool big = (o.mode == "bulk" || o.mode == "bulkget");
    int count = big ? 64 : 1024;
    for (int i = 0; i < count; i++) {
        if (o.mode == "health") pool.push_back(make_req("GET", "/health", ""));
        else if (o.mode == "get") pool.push_back(make_req("GET", "/items/" + std::to_string(xs(s) % o.keyspace), ""));
        else if (o.mode == "put") pool.push_back(make_req("PUT", "/items/" + std::to_string(xs(s) % o.keyspace), "hello-bench-value-0123456789"));
        else if (o.mode == "bulk") pool.push_back(make_req("POST", "/items/bulk", bulk_body(s, o.keyspace, o.batch)));
        else if (o.mode == "bulkget") pool.push_back(make_req("POST", "/items/bulk/get", ids_body(s, o.keyspace, o.batch)));
        else { fprintf(stderr, "unknown mode %s\n", o.mode.c_str()); exit(2); }
    }
    return pool;
}

static int dial(const Opts& o) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(o.port);
    if (inet_pton(AF_INET, o.host.c_str(), &a.sin_addr) != 1) { fprintf(stderr, "bad host (IPv4 literal only)\n"); exit(2); }
    if (connect(fd, (sockaddr*)&a, sizeof a) < 0) { perror("connect"); exit(1); }
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    return fd;
}

// ---- response parsing ----
static long find_cl(const char* h, size_t n) {
    const char* end = h + n;
    for (const char* p = (const char*)memchr(h, '\n', n); p && p + 1 < end; ) {
        p++;
        if ((size_t)(end - p) > 15 && strncasecmp(p, "content-length:", 15) == 0) return atol(p + 15);
        p = (const char*)memchr(p, '\n', end - p);
    }
    return 0;
}

// Try to consume one response from buf[0..len). Returns bytes consumed, 0 if incomplete.
static size_t parse_resp(const char* buf, size_t len, int& status, size_t& need) {
    const char* he = (const char*)memmem(buf, len, "\r\n\r\n", 4);
    if (!he) { need = len + 1; return 0; }
    size_t hlen = (he - buf) + 4;
    long cl = find_cl(buf, hlen);
    if (len < hlen + cl) { need = hlen + cl; return 0; }
    status = len >= 12 ? atoi(buf + 9) : 0;
    return hlen + cl;
}

static void preload(const Opts& o) {
    int fd = dial(o);
    u64 s = 12345;
    std::vector<char> rb(1 << 20);
    const u64 chunk = 1000;
    for (u64 i = 0; i < o.keyspace; i += chunk) {
        std::string body;
        for (u64 k = i; k < std::min(o.keyspace, i + chunk); k++)
            body += "{\"id\":" + std::to_string(k) + ",\"v\":\"value-0123456789abcdef\"}\n";
        std::string req = make_req("POST", "/items/bulk", body);
        for (size_t off = 0; off < req.size();) { ssize_t n = send(fd, req.data() + off, req.size() - off, 0); if (n <= 0) { perror("preload send"); exit(1); } off += n; }
        size_t len = 0, need = 0; int st = 0;
        for (;;) {
            ssize_t n = recv(fd, rb.data() + len, rb.size() - len, 0);
            if (n <= 0) { perror("preload recv"); exit(1); }
            len += n;
            if (parse_resp(rb.data(), len, st, need)) break;
        }
        if (st != 200) { fprintf(stderr, "preload failed: HTTP %d\n", st); exit(1); }
    }
    (void)s;
    close(fd);
}

struct BConn {
    int fd = -1;
    std::vector<char> rbuf = std::vector<char>(64 * 1024);
    size_t rlen = 0;
    std::string wbuf;
    size_t woff = 0;
    int inflight = 0;
    u64 t_send = 0;
    size_t cursor = 0;
};

struct Result { Hist h; u64 reqs = 0, errs = 0, bytes = 0, measured_ns = 0; };

static void worker(const Opts& o, int tid, int nconns, Result& out) {
    auto pool = build_pool(o, tid + 1);
    int ep = epoll_create1(0);
    std::vector<BConn> cs(nconns);
    auto fill = [&](BConn& c) {
        c.wbuf.clear();
        c.woff = 0;
        for (int i = 0; i < o.pipeline; i++) c.wbuf += pool[c.cursor++ % pool.size()];
        c.inflight = o.pipeline;
        c.t_send = now_ns();
    };
    for (int i = 0; i < nconns; i++) {
        BConn& c = cs[i];
        c.fd = dial(o);
        fcntl(c.fd, F_SETFL, O_NONBLOCK);
        c.cursor = (u64)i * 7 + tid * 13;
        epoll_event ev{};
        ev.events = EPOLLIN | EPOLLOUT;
        ev.data.ptr = &c;
        epoll_ctl(ep, EPOLL_CTL_ADD, c.fd, &ev);
        fill(c);
    }
    u64 start = now_ns(), warm_end = start + (u64)o.warm * 1000000000ull, end = warm_end + (u64)o.secs * 1000000000ull;
    u64 meas_start = 0;
    epoll_event evs[256];
    while (true) {
        u64 t = now_ns();
        if (t >= end) break;
        int n = epoll_wait(ep, evs, 256, 50);
        for (int i = 0; i < n; i++) {
            BConn& c = *(BConn*)evs[i].data.ptr;
            if (evs[i].events & (EPOLLERR | EPOLLHUP)) { fprintf(stderr, "connection dropped\n"); out.errs++; epoll_ctl(ep, EPOLL_CTL_DEL, c.fd, nullptr); continue; }
            if ((evs[i].events & EPOLLOUT) && c.woff < c.wbuf.size()) {
                ssize_t w = send(c.fd, c.wbuf.data() + c.woff, c.wbuf.size() - c.woff, MSG_NOSIGNAL);
                if (w > 0) c.woff += w;
                if (c.woff >= c.wbuf.size()) {
                    epoll_event ev{}; ev.events = EPOLLIN; ev.data.ptr = &c;
                    epoll_ctl(ep, EPOLL_CTL_MOD, c.fd, &ev);
                }
            }
            if (evs[i].events & EPOLLIN) {
                if (c.rlen == c.rbuf.size()) c.rbuf.resize(c.rbuf.size() * 2);
                ssize_t r = recv(c.fd, c.rbuf.data() + c.rlen, c.rbuf.size() - c.rlen, 0);
                if (r <= 0) { if (r < 0 && errno == EAGAIN) continue; out.errs++; epoll_ctl(ep, EPOLL_CTL_DEL, c.fd, nullptr); continue; }
                c.rlen += r;
                size_t off = 0;
                bool measuring = now_ns() >= warm_end;
                if (measuring && !meas_start) meas_start = now_ns();
                for (;;) {
                    int st = 0; size_t need = 0;
                    size_t used = parse_resp(c.rbuf.data() + off, c.rlen - off, st, need);
                    if (!used) {
                        if (need > c.rbuf.size()) c.rbuf.resize(need + off);
                        break;
                    }
                    off += used;
                    if (measuring) { out.reqs++; out.bytes += used; if (st < 200 || st >= 300) out.errs++; }
                    c.inflight--;
                }
                if (off) { memmove(c.rbuf.data(), c.rbuf.data() + off, c.rlen - off); c.rlen -= off; }
                if (c.inflight == 0) {
                    if (measuring) out.h.add(now_ns() - c.t_send);
                    fill(c);
                    ssize_t w = send(c.fd, c.wbuf.data(), c.wbuf.size(), MSG_NOSIGNAL);
                    if (w > 0) c.woff = w;
                    if (c.woff < c.wbuf.size()) {
                        epoll_event ev{}; ev.events = EPOLLIN | EPOLLOUT; ev.data.ptr = &c;
                        epoll_ctl(ep, EPOLL_CTL_MOD, c.fd, &ev);
                    }
                }
            }
        }
    }
    out.measured_ns = meas_start ? now_ns() - meas_start : 0;
    for (auto& c : cs) close(c.fd);
}

int main(int argc, char** argv) {
    Opts o;
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        auto next = [&]() -> const char* { if (i + 1 >= argc) { fprintf(stderr, "missing value for %s\n", a.c_str()); exit(2); } return argv[++i]; };
        if (a == "-h" || a == "--host") o.host = next();
        else if (a == "-p" || a == "--port") o.port = atoi(next());
        else if (a == "-m" || a == "--mode") o.mode = next();
        else if (a == "-c" || a == "--conns") o.conns = atoi(next());
        else if (a == "-t" || a == "--threads") o.threads = atoi(next());
        else if (a == "-d" || a == "--secs") o.secs = atoi(next());
        else if (a == "-w" || a == "--warmup") o.warm = atoi(next());
        else if (a == "-P" || a == "--pipeline") o.pipeline = std::max(1, atoi(next()));
        else if (a == "-n" || a == "--batch") o.batch = std::max(1, atoi(next()));
        else if (a == "-K" || a == "--keyspace") o.keyspace = strtoull(next(), nullptr, 10);
        else if (a == "--no-preload") o.preload = false;
        else {
            fprintf(stderr, "usage: %s [-h host] [-p port] [-m health|get|put|bulk|bulkget] [-c conns] [-t threads]\n"
                            "          [-d secs] [-w warmup] [-P pipeline] [-n batch] [-K keyspace] [--no-preload]\n", argv[0]);
            return 2;
        }
    }
    o.threads = std::max(1, std::min(o.threads, o.conns));
    if (o.preload && (o.mode == "get" || o.mode == "bulkget")) {
        fprintf(stderr, "preloading %llu keys...\n", (unsigned long long)o.keyspace);
        preload(o);
    }
    printf("mode=%s conns=%d threads=%d pipeline=%d batch=%d duration=%ds (+%ds warmup)\n",
           o.mode.c_str(), o.conns, o.threads, o.pipeline, o.batch, o.secs, o.warm);

    std::vector<Result> res(o.threads);
    std::vector<std::thread> ts;
    for (int t = 0; t < o.threads; t++) {
        int share = o.conns / o.threads + (t < o.conns % o.threads);
        ts.emplace_back(worker, std::cref(o), t, share, std::ref(res[t]));
    }
    for (auto& t : ts) t.join();

    Hist h; u64 reqs = 0, errs = 0, bytes = 0, ns = 0;
    for (auto& r : res) { h.merge(r.h); reqs += r.reqs; errs += r.errs; bytes += r.bytes; ns = std::max(ns, r.measured_ns); }
    double secs = ns / 1e9;
    if (secs <= 0) { fprintf(stderr, "no measurements (server unreachable?)\n"); return 1; }
    double rps = reqs / secs;
    printf("requests:   %llu in %.2fs\n", (unsigned long long)reqs, secs);
    printf("throughput: %.0f req/s", rps);
    if (o.mode == "bulk" || o.mode == "bulkget") printf("  (%.0f items/s)", rps * o.batch);
    printf("\nread:       %.2f MB/s\n", bytes / secs / 1e6);
    printf("errors:     %llu\n", (unsigned long long)errs);
    printf("latency per %s (us): avg %.0f  p50 %.0f  p90 %.0f  p99 %.0f  p99.9 %.0f  max %.0f\n",
           o.pipeline > 1 ? "pipelined batch" : "request",
           h.n ? h.sum / 1e3 / h.n : 0.0, h.pct(50) / 1e3, h.pct(90) / 1e3, h.pct(99) / 1e3, h.pct(99.9) / 1e3, h.max / 1e3);
    return errs ? 1 : 0;
}
