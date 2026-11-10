# bulkapi

C++20 bulk REST server on io_uring with an epoll fallback,
plus a C++ and a Python load generator. Routes are defined in `api.spec` and compiled
into the server at build time.

    make                                   # runs tools/gen_routes.py on api.spec, then builds
    build/server -p 8080 [-t N] [--backend auto|uring|uring-ms|epoll] [--pin]
    build/bench  -m get -c 256 -t 2 -d 10 -P 8        # modes: health get put bulk bulkget
    python3 bench/bench.py -m bulk -n 500 -c 16       # simple stdlib version

Backends: `uring` (raw syscalls, single-shot receive; the default, falls back to epoll), `uring-ms`
(liburing, `io_uring_prep_recv_multishot` into a provided-buffer ring, Linux 6.1+, no fallback) and `epoll`.
The build needs liburing 2.3 or newer; the Makefile explains how to install it and where it looks.
A Java port with io_uring, epoll and blocking-socket backends is in `../bulkapi-java`.

To add an endpoint: add a line to `api.spec`, implement `void h_<name>(const Req&, Res&)`
in `src/handlers.cpp`, run `make` (a spec route without a handler fails at link time).

API (NDJSON for bulk): `POST /items/bulk` body lines `{"id":1,"v":"text"}`;
`POST /items/bulk/get` and `/items/bulk/delete` body = ids separated by newlines/spaces.
Also `GET|PUT|DELETE /items/:id`, `GET /health`, `GET /stats`.

Notes: chunked request bodies are rejected (501); send Content-Length. `v` is a flat JSON
string; the parser is intentionally minimal, not a full JSON parser.
