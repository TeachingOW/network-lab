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

Two rounds each, range shown. 4 worker threads, `build/bench -t 4 -d 4 -w 3` on the same 4-core laptop
(WSL2), JDK 11, `-Xms1g -Xmx2g -XX:+UseParallelGC`. All runs had 0 errors. Requests per second:

| Workload | C++ uring | Java uring | Java epoll | Java sockets |
|---|---|---|---|---|
| get, 256 conns, no pipelining | 251-421k | 267-271k | 217-223k | 73-132k |
| get, 256 conns, pipeline 8 | 1.04-1.35M | 1.20-1.22M | 0.71-0.94M | 0.53-0.68M |
| bulk insert, 64 conns, 100 items | 46-60k | 55-65k | 46-50k | 55-56k |
| bulk get, 64 conns, 100 items | 43-54k | 62-63k | 46-47k | 55-58k |
| process memory after the run | 20 MB | 349-364 MB | 335-346 MB | 396-400 MB |

- Among the Java backends, `uring` is fastest on every workload and `epoll` is about 20% behind.
  `sockets` holds up on bulk requests but drops to a quarter to a half of `uring` on small unpipelined
  requests, with a p99 three to eight times higher (256 connections means 256 threads on 4 cores).
- Against the C++ server, Java `uring` is level within this machine's noise, at about 18 times the
  memory. Most of that is the Java heap, sized at 1 GB for the test.

The load generator shares the cores with the server and the same binary varies by up to 2x between runs
here, so differences under about 1.5x should not be read as real. An earlier single-run table showed
Java `uring` behind Java `epoll` on small requests; that did not hold over two rounds.

A per-request CPU comparison of all five backends (Java and C++) is in [JAVA_COMPARISON.md](JAVA_COMPARISON.md).
