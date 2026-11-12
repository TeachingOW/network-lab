# bulkapi

C++20 bulk REST server on io_uring (or epoll, when asked for),
plus a C++ and a Python load generator. Routes are defined in `api.spec` and compiled
into the server at build time.

    make                                   # runs tools/gen_routes.py on api.spec, then builds
    build/server -p 8080 [-t N] [--backend uring|uring-ms|epoll] [--pin]
                 [--batch-wait-us N] [--idle-timeout S] [--header-timeout S] [--body-timeout S] [--write-timeout S]
    build/bench  -m get -c 256 -t 2 -d 10 -P 8        # modes: health get put bulk bulkget
    python3 bench/bench.py -m bulk -n 500 -c 16       # simple stdlib version

Backends: `uring` (raw syscalls, single-shot receive; the default), `uring-ms`
(liburing, `io_uring_prep_recv_multishot` into a provided-buffer ring; connections hold
no buffer while idle and requests are parsed in place in the shared buffer) and `epoll`. Both io_uring
backends need Linux 6.1+. There is no fallback: if the chosen backend cannot start, the server exits. `uring-ms` suits
thousands of connections; `uring` is cheaper per request up to a few hundred (see RESULTS.md).
The build needs liburing 2.3 or newer; the Makefile explains how to install it and where it looks.
A Java port with io_uring, epoll and blocking-socket backends is in `../bulkapi-java`.

Timeouts (seconds, fractions allowed, 0 disables one), in every backend:

| Flag | Default | Applies while | On expiry |
|---|---|---|---|
| `--idle-timeout` | 60 | a keep-alive connection holds no input | closed |
| `--header-timeout` | 10 | a request header is incomplete; counted from its first byte, not extended by more bytes | 408, closed |
| `--body-timeout` | 30 | the body is incomplete; restarted whenever more of it arrives | 408, closed |
| `--write-timeout` | 30 | a response is unsent; restarted whenever the send makes progress | closed |

Each worker keeps one list per timeout, ordered by deadline (every entry in a list got the same
duration, so appending keeps the order), and waits in the kernel only until the earliest deadline.
A worker with no deadlines pending does not wake up at all.

To add an endpoint: add a line to `api.spec`, implement `void h_<name>(const Req&, Res&)`
in `src/handlers.cpp`, run `make` (a spec route without a handler fails at link time).

API (NDJSON for bulk): `POST /items/bulk` body lines `{"id":1,"v":"text"}`;
`POST /items/bulk/get` and `/items/bulk/delete` body = ids separated by newlines/spaces.
Also `GET|PUT|DELETE /items/:id`, `GET /health`, `GET /stats`.

Notes: chunked request bodies are rejected (501); send Content-Length. `v` is a flat JSON
string; the parser is intentionally minimal, not a full JSON parser.
