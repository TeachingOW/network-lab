# Java backends compared with each other and with the C++ server

Measured 2026-11-10 on one 4-core laptop (Intel i5-1145G7, WSL2, Linux 6.18), load generator
(`../bulkapi/build/bench -t 4`) on the same machine. JDK 11, `-Xms1g -Xmx2g -XX:+UseParallelGC`.

## Design

| | C++ `uring` | C++ `uring-ms` | Java `uring` | Java `epoll` | Java `sockets` |
|---|---|---|---|---|---|
| Waiting for data | Receive re-armed per request | One receive stays armed (multishot) | Receive re-armed per request | Selector (epoll), then a read call | Thread blocks in `read()` |
| Threads | 1 per core | 1 per core | 1 per core | 1 per core | 1 per connection |
| Kernel interface | Raw syscalls | liburing | liburing for setup; Java drives the queues in shared memory | JDK NIO | JDK NIO |
| Input buffer | 16 KB per connection | Shared pool, parsed in place | 16 KB off-heap per connection | 16 KB off-heap per connection | 16 KB off-heap per connection |
| Store | Locked `unordered_map` | Same | Open addressing over flat arrays | Same | Same |
| Response | Written once | Written once | Written once | Written once | Written once |
| `\u` escapes, header trimming | Buggy | Buggy | Correct | Correct | Correct |
| Memory after a benchmark run | 20 MB | not measured | ~350 MB | ~340 MB | ~400 MB |

## CPU per request, one server thread

Server CPU time divided by requests completed, 64 connections, one run each. Java figures include the
JVM's garbage-collector and compiler threads.

| Workload | C++ `uring` | C++ `uring-ms` | Java `uring` | Java `epoll` | Java `sockets`* |
|---|---|---|---|---|---|
| GET, no pipelining | **6.3 µs** | 10.9 µs | 10.5 µs | 16.5 µs | 27.3 µs |
| GET, pipeline of 8 | 2.1 µs | 2.2 µs | **2.0 µs** | 2.8 µs | 6.6 µs |
| Bulk insert, 100 items | 32.5 µs | 30.5 µs | **27.4 µs** | 45.6 µs | 81.8 µs |
| Bulk get, 100 items | 51.0 µs | 52.8 µs | **36.4 µs** | 55.9 µs | 83.5 µs |

\* `sockets` uses one thread per connection, so it cannot be limited to one thread and ran on all four
cores. That makes its throughput look highest, but it is the most expensive per request on every workload.

Throughput with four server threads (two rounds, range) is in [README.md](README.md#benchmark).

## What it shows

- **Among the Java backends,** io_uring is cheapest on every workload. epoll costs 1.4–1.7x as much and
  blocking sockets 2.3–3.3x.
- **Java io_uring against C++:** C++ single-shot is clearly cheaper on small unpipelined requests
  (6.3 against 10.5 µs), where the JVM's overhead is not hidden by the work. On pipelined requests and
  bulk insert the two are level.
- **Bulk get:** Java io_uring is about 1.4x cheaper than either C++ backend. Java already has two of the
  C++ server's open optimisations, the flat-array store and escaping that copies clean runs in bulk. The
  reference server without SQLite showed the same gap (`../reference-nodb/README.md`), so the store is
  the next thing to change in C++.
- **Memory** is the cost of Java: about 18 times the C++ server's, mostly the heap sized at 1 GB for
  these runs. How far it can shrink was not tested.

Single runs on a machine where the same binary varies by up to 2x between runs: differences under
about 1.5x should not be read as real.
