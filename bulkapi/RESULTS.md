# Benchmark results

Measured 2026-11-10 with `build/bench` against `build/server` on the same machine over loopback.

- Machine: Intel Core i5-1145G7 (4 cores / 8 threads), 8 GB RAM, WSL2, Linux 6.18
- Compiler: g++ 14.3, `-O3 -march=native`
- Server: `build/server -t 4 [--backend uring|epoll]`
- Load: `build/bench -t 4 -d 10` (10 s measured after 1 s warmup)
- Every run finished with 0 errors.

| Backend | Mode | Conns | Pipeline | Throughput | p50 | p99 |
|---|---|---|---|---|---|---|
| io_uring | health | 256 | 8 | 1,263,271 req/s | 0.85 ms | 12.1 ms |
| io_uring | get | 256 | 8 | 1,099,624 req/s | 1.18 ms | 11.5 ms |
| io_uring | get | 256 | 1 | 209,898 req/s | 0.69 ms | 9.4 ms |
| io_uring | put | 256 | 8 | 1,151,047 req/s | 1.11 ms | 11.0 ms |
| io_uring | bulk (100 items/request) | 64 | 1 | 32,738 req/s = 3.27M items/s | 1.11 ms | 16.8 ms |
| io_uring | bulkget (100 items/request) | 64 | 1 | 40,114 req/s = 4.01M items/s | 1.18 ms | 9.4 ms |
| epoll | get | 256 | 8 | 977,124 req/s | 1.31 ms | 13.6 ms |
| epoll | get | 256 | 1 | 173,943 req/s | 0.79 ms | 11.5 ms |

For pipelined runs the latency is per batch of 8 requests, not per request.

## Reading the numbers

- io_uring is about 13% faster than epoll on pipelined gets and about 21% faster unpipelined.
- Pipelining 8 requests gives roughly 5x the request rate of one request at a time.
- The bulk endpoints move 3-4x more items per second than single-item requests.

## Caveats

- The load generator shares the 4 cores with the server, so tail latencies mostly reflect CPU contention.
- This is a laptop under WSL2. Repeated runs of the same binary varied by up to 2x (for example
  pipelined get between about 320k and 1.15M req/s), so treat these as rough figures, not a
  stable baseline.
- These runs used the single-file build, before the server was split into components. The split
  build returned identical responses and showed no measurable speed difference within that noise.

## Multishot receive (`--backend uring-ms`) against single-shot (`--backend uring`)

Same machine and settings, 5 s per run, three alternating rounds, median shown. All runs had 0 errors.

| Mode | Conns | Pipeline | uring | uring-ms |
|---|---|---|---|---|
| get | 256 | 8 | 1,563,660 req/s | 1,504,932 req/s |
| get | 256 | 1 | 301,819 req/s | 318,900 req/s |
| put | 256 | 8 | 1,712,147 req/s | 1,790,251 req/s |
| bulk (100 items/request) | 64 | 1 | 90,865 req/s | 86,853 req/s |
| bulkget (100 items/request) | 64 | 1 | 61,972 req/s | 61,227 req/s |

This table was taken with the first, raw-syscall version of `uring-ms`. The backend was then rewritten
on liburing 2.15; a shorter re-run of it gave the same picture (for example 1.48M req/s on pipelined get).

The differences are within the run-to-run spread on this machine (single rounds ranged from 1.29M to
2.14M req/s on pipelined get), so neither backend is measurably faster here. These runs were taken at
a different time from the table above and are only comparable with each other.

## Reproduce

    make
    build/server -p 8080 -t 4 &
    build/bench -p 8080 -t 4 -d 10 -m get -c 256 -P 8
    build/bench -p 8080 -t 4 -d 10 -m bulk -c 64 -n 100
