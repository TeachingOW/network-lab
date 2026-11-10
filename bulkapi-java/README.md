# bulkapi-java

The bulkapi server in Java: same API, same `api.spec`, three I/O backends.

    make                      # javac + the JNI shim (needs JDK 11+ and liburing 2.3+)
    java -XX:+UseParallelGC -Djava.library.path=build -cp build/classes bulkapi.Server \
         -p 8080 [-t N] [--backend uring|epoll|sockets] [--spec ../bulkapi/api.spec]

The load generators in `../bulkapi` work against it unchanged.

| Backend | How it does I/O |
|---|---|
| `uring` (default) | io_uring. liburing sets the ring up through a small JNI shim (`native/uring_jni.c`); Java then writes submissions and reads completions directly in the rings' shared memory, so the only JNI call on the hot path is `io_uring_enter`, once per loop turn. Multishot accept, one recv or send in flight per connection. Needs Linux 6.1+. |
| `epoll` | `java.nio` selectors, which are epoll on Linux. One selector and one `SO_REUSEPORT` listener per thread, the allocation-free `select(Consumer)` form, write interest only when a send would block. |
| `sockets` | Blocking sockets, one pooled thread per connection. |

## What is optimised

- **Direct buffers end to end.** The kernel reads into and writes from off-heap buffers; requests are
  parsed in place with no `String` or per-request objects.
- **Responses are written once.** A handler appends its body straight into the output buffer after a
  reserved gap, and the header is filled in afterwards. Small bodies are moved down against the header;
  bodies over 4 KB stay put and the header is padded with spaces instead.
- **Primitive store.** 256 shards, each an open-addressing table over a `long[]` and a `byte[][]`:
  no boxing and no node per entry.
- **Spec-driven routing.** `api.spec` is read at startup; a route whose handler is missing from
  `Handlers.java` stops the server from starting.

## Differences from the C++ server

- `\uXXXX` escapes in bulk-insert values are decoded to UTF-8 (the C++ server drops the backslash).
- Header values are trimmed, so `Content-Length: 2 ` with a trailing space is accepted (C++ answers 400).
- Large responses carry a space-padded `Content-Length` header.
- Routes are loaded at startup instead of being generated at build time.

Everything else returned byte-identical responses to the C++ server in a 29-line probe on all three
backends, including a 20 MB bulk upload and a 50,000-item bulk get.

## Benchmark

One run each, 4 worker threads, `build/bench -t 4 -d 5 -w 3` on the same 4-core laptop (WSL2), JDK 11,
`-Xms1g -Xmx2g -XX:+UseParallelGC`. All runs had 0 errors. Requests per second:

| Workload | C++ uring | C++ epoll | Java uring | Java epoll | Java sockets |
|---|---|---|---|---|---|
| get, 256 conns, pipeline 8 | 1,137,129 | 1,011,079 | 623,585 | 899,082 | 855,987 |
| get, 256 conns, no pipelining | 191,298 | 244,645 | 110,560 | 195,489 | 110,805 |
| put, 256 conns, pipeline 8 | 1,248,956 | 1,257,316 | 884,376 | 911,146 | 615,767 |
| bulk insert, 64 conns, 100 items | 37,839 | 34,749 | 41,116 | 51,384 | 70,826 |
| bulk get, 64 conns, 100 items | 25,448 | 44,774 | 59,887 | 49,838 | 74,299 |

Treat this as a smoke test, not a ranking. It is a single run per cell on a machine where the same
binary varies by up to 2x between runs (the C++ bulk figures here are less than half of what the same
build measured an hour earlier, see `../bulkapi/RESULTS.md`), and the load generator shares the cores
with the server. The one pattern that held across workloads is that the C++ server is ahead on small
single requests.
