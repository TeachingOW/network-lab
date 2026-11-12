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

## No per-connection buffer, no response copy (later change)

Two changes, measured with one server thread (`-t 1`) so that CPU per request is meaningful; the load
generator (`bench -t 4`) shares the machine. Two rounds each, both shown where they differ.

1. `uring-ms` no longer gives a connection its own input buffer. A request is parsed in place in the
   shared receive buffer; a private buffer exists only while a request is incomplete or input arrives
   during a send. Output buffers are borrowed from a per-worker pool while a reply exists.
2. All backends write a response body once, straight into the output buffer, instead of building it
   and copying it.

Memory, 20,000 connections that each made one request and then sat idle (process RSS growth):

| Backend | Per connection |
|---|---|
| `uring` | 4.33 KB |
| `uring-ms` before | 4.54 KB |
| `uring-ms` now | 0.30 KB |

Server CPU per request, get, no pipelining:

| Connections | `uring` | `uring-ms` |
|---|---|---|
| 64 | 13-14 us | 15.6-15.8 us |
| 256 | 8.8-9.8 us | 15.5-19.5 us |
| 1,000 | 22 us | 15-18 us |
| 2,000 | 27-28 us | 14.5-14.7 us |

`uring-ms` costs about the same at every connection count; `uring` is cheaper up to a few hundred
connections and about twice as expensive at 2,000. The same pattern is in the version before this change
(15.1 us at 256 connections), so it comes from multishot receive itself, not from removing the buffer.

Removing the response copy changed nothing measurable: bulk get of 100 items cost 44.6 and 46.0 us per
request before and 40.9 and 44.8 after, and bulk get of 1,000 items was unchanged. Responses are
byte-identical to before on a 34-line probe for all three backends, except that bodies over 4 KB now
carry a space-padded `Content-Length` header.

`BULKAPI_BATCH_US=N` makes `uring-ms` wait up to N microseconds for more completions when the previous
turn handled 32 or more. At 200 and 1,000 us it showed no consistent gain at 16, 256 or 2,000
connections, so it is off by default.

## Timeouts, store rewrite and per-request trimming (2026-11-12)

Changes, all backends unless noted:

- **Timeouts** (`--idle-timeout` 60 s, `--header-timeout` 10 s, `--body-timeout` 30 s, `--write-timeout` 30 s;
  see README). One FIFO list per timeout per worker, so a refresh is O(1) and expiry looks only at
  list heads; the loop waits in the kernel only until the earliest deadline (no wake-ups at all while
  nothing is due: 0 context switches in 3 s with 1,200 idle connections). A half-received request gets
  a 408. Before this, a connection could stay open forever and a request announcing a 64 MB body
  reserved 64 MB at once (200 such connections: 12.9 GB of address space before, 64 MB now).
- **Store**: `std::unordered_map<u64, std::string>` replaced by open addressing with linear probing,
  64-byte slots with values up to 48 bytes inline, backward-shift deletion. One cache line per lookup.
- **JSON**: escaping scans 16 bytes at a time (SSE2) and appends unescaped runs in one call; no
  `snprintf`; unescaping copies runs between backslashes. Bulk get reserves its output once.
- **HTTP**: method parsed once; status line and content type come from a prebuilt prefix.
- `uring`: receives use `IORING_RECVSEND_POLL_FIRST` (skips the receive attempt that would fail).
- `uring` and `uring-ms`: batch wait on by default (`--batch-wait-us 200`, replaces the
  `BULKAPI_BATCH_US` variable): after a turn with 32+ completions, wait up to 200 us for more.
- `uring-ms`: receive buffers prefaulted.

Checked: responses byte-identical to the previous build on all three backends (`tests/probe.py`, 661
lines incl. escaping, 20,000-item insert/delete/get); `tests/timeouts.py` 8/8 on each backend; a
4-thread mixed load under ASan and UBSan with no reports.

Method (`bench/compare.py`): every server on one pinned CPU (`-t 1`; the reference is single-threaded),
the load generator on other CPUs, CPU per request read from `/proc/<pid>/stat` over the measured window.
Five rounds, server order rotated each round, 4 s per run, median shown. 175 runs per machine, 0 errors.
"before" is commit 337d3a1, "ref" is `../reference-nodb` with its defaults.

**x-02** (Xeon Gold 5220R VM, 90 vCPUs, Linux 6.8, native; quiet), server CPU per request in us:

| Workload | ref | uring before | uring now | uring-ms before | uring-ms now | best now vs ref |
|---|---|---|---|---|---|---|
| get, 16 conns | 22.32 | 10.53 | 10.77 | 10.48 | 10.35 | 2.16x less |
| get, 256 conns | 9.77 | 9.78 | 9.50 | 9.72 | 9.76 | 1.03x less |
| get, 2,000 conns | 11.31 | 11.05 | 11.55 | 10.09 | **9.50** | **1.19x less** |
| get, 256 conns, pipeline 8 | 9.81 | 1.77 | 1.66 | 1.54 | 1.45 | 6.8x less |
| put, 256 conns, pipeline 8 | 9.33 | 1.64 | 1.68 | 1.48 | 1.46 | 6.4x less |
| bulk insert, 100 items | 60.59 | 27.08 | 20.24 | 26.71 | 20.01 | 3.0x less |
| bulk get, 100 items | 22.97 | 34.39 | **23.04** | 33.53 | 23.29 | level |

Throughput on x-02 (req/s): get at 2,000 conns 105k (`uring-ms`) against 88k; bulk get 43.4k against 43.5k;
get at 16 conns 97k against 6.8k (the reference's 2 ms batch wait).

**x-01** (i5-1145G7, WSL2, Linux 6.18; noisy: single runs of one configuration differ by up to 2.3x):

| Workload | ref | uring before | uring now | uring-ms before | uring-ms now | best now vs ref |
|---|---|---|---|---|---|---|
| get, 16 conns | 17.83 | 13.21 | 16.49 | 16.43 | 16.42 | 1.09x less |
| get, 256 conns | 9.74 | 12.15 | **8.25** | 14.14 | 13.21 | 1.18x less |
| get, 2,000 conns | **13.08** | 26.36 | 17.60 | 14.93 | 14.60 | **0.90x (ref ahead)** |
| get, 256 conns, pipeline 8 | 11.78 | 1.38 | 2.15 | 2.02 | 1.80 | 6.5x less |
| put, 256 conns, pipeline 8 | 13.63 | 1.75 | 1.34 | 1.81 | 1.78 | 10x less |
| bulk insert, 100 items | 41.87 | 17.65 | 17.81 | 25.00 | 22.63 | 2.4x less |
| bulk get, 100 items | 24.73 | 39.09 | **22.49** | 40.47 | 25.72 | 1.10x less |

What changed what:

- **Bulk get, 1.5x cheaper** (34 -> 23 us on x-02, 39 -> 22.5 on x-01): the store and the JSON writer.
  It was the reference's clearest lead; it is now level (x-02) or slightly behind us (x-01).
- **Bulk insert, 1.3x cheaper** (27 -> 20 us): no `std::string` node per item, run-wise unescaping.
- **`uring` at many connections on x-01** (26 -> 17.6 us at 2,000): `POLL_FIRST`. With it off and
  everything else equal, 28-31 us; on, 13.5-17 us. On x-02 the old code had no such penalty and the flag
  changes nothing measurable, so this looks specific to the WSL kernel.
- **`uring-ms` at 2,000 connections** (10.1 -> 9.5 us on x-02): the batch wait. A sweep on x-02 with
  0/50/200/1000 us put 200 us best at 256 and 2,000 connections and showed no cost at 16.
- **Small gets are ~95% kernel time** in all three servers (x-02, 256 connections: user 0.35 us for the
  reference, 0.45 us for us, system 8.4 us for both), so user-space work cannot move them further.
  Remaining levers are kernel-side: direct descriptors and fewer operations per request.

Where the reference is still ahead: 2,000 connections on x-01 (13.1 against 14.6 us, about 10%, inside
that machine's noise but in both full runs); idle memory (0 against 0.30 KB per connection for `uring-ms`).

Reproduce on a machine with both trees built (see the header of `bench/compare.py`):

    python3 bench/compare.py ROOT out.jsonl --rounds 5 --secs 4 --server-cpu 0 --bench-cpus 2-7 --bench-threads 4
    python3 bench/summarise.py out.jsonl

## Reproduce

    make
    build/server -p 8080 -t 4 &
    build/bench -p 8080 -t 4 -d 10 -m get -c 256 -P 8
    build/bench -p 8080 -t 4 -d 10 -m bulk -c 64 -n 100
