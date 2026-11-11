# reference-nodb

The reference server (`../reference/c-iouring-pavlyhalim`, commit `6a1eab9`) with SQLite, the B-tree
read plans, the group commit, the checkpoint thread and JWT removed, serving bulkapi's API from an
in-memory hash table. It exists to compare the reference's *event loop* with bulkapi's on the same
workload and the same load generator, without the database in the way.

    make                                  # needs liburing's header, see ../bulkapi/Makefile
    HOST=127.0.0.1 PORT=8080 build/server
    ../bulkapi/build/bench -p 8080 -m get -c 256 -P 1

## What was kept, changed and removed

| File | Status |
|---|---|
| `src/server.c` | The reference's loop, unchanged in behaviour: one thread, multishot poll, bounded read queue, shared 1 KB receive buffers, direct descriptors, batch arenas, 64-completions-or-2-ms wait, one answer per connection per batch, idle sweep. Removed: the SQLite include, the commit, the write gate's database check, the SQLite statistics and the PGO training database. |
| `src/sys.h` | Unchanged except that the io_uring ABI comes from liburing's header (5.15 kernel headers lack buffer rings). Still raw syscalls. |
| `src/http.h` | Unchanged except that `PUT` and `DELETE` are recognised. |
| `src/json.h` | Unchanged. |
| `src/app.h` | New: bulkapi's eight endpoints on the reference's response plumbing (body written once into the arena, constant error responses). |
| `src/store.h` | New: open-addressing hash table, no locks (one thread owns it). |
| removed | `db.h`, `btree.h`, `jwt.h`, `sha256.h`, `train.c`, the SQLite amalgamation. |

## Limits inherited from the reference

- Request bodies up to 1 MB (bulkapi: 64 MB on bulk insert); a larger one is answered 400 and closed.
- Responses up to 64 KB; a larger bulk get is answered 413.
- `Expect: 100-continue` is not answered, so curl waits a second before sending a large body.
- A constant error response on a closing connection carries no `Connection: close` header.

Against bulkapi it returned the same responses on 27 of 30 probe lines. The three differences are the
400-instead-of-413 above, the missing `Connection: close`, and `\uXXXX` being decoded correctly here.

## Results against bulkapi

4-core laptop under WSL2, load generator on the same machine (`bench -t 4`, 5 s runs). `bulkapi x1` is
`--backend uring -t 1`, the like-for-like comparison with this single-threaded server; `bulkapi x4`
uses four threads. Median of three alternating rounds unless a CPU figure is shown; 0 errors everywhere.

| Workload | reference-nodb | bulkapi x1 | bulkapi x4 |
|---|---|---|---|
| get, 16 conns, no pipelining | 6,549 req/s (p50 2.4 ms) | 59,367 (p50 0.25 ms) | 92,821 |
| get, 256 conns, no pipelining | 94,343 | 61,385 | 238,622 |
| get, 2,000 conns, no pipelining | 57,802 | 30,817 | 208,734 |
| get, 256 conns, pipeline 8 | 68,157 | 438,789 | 1,435,574 |
| put, 256 conns, pipeline 8 | 52,147 | 812,569 | 1,495,238 |
| bulk insert, 100 items (1 KB receive buffers) | 4,655 (p50 13.6 ms) | 41,913 | 75,772 |
| bulk insert, 100 items (`RECV_BUF_SIZE=16384`) | 33,738 | 35,883 | - |
| bulk get, 100 items | 33,831 | 20,288 | 54,183 |

Server CPU per request, single-threaded servers, both at about 95% of one core:

| Workload | reference-nodb | bulkapi x1 |
|---|---|---|
| get, 256 conns | 10.5 µs | 9.7 µs |
| get, 2,000 conns | 15.6 µs | 29.4 µs |
| bulk insert, 16 KB buffers | 28.6 µs | 27.0 µs |
| bulk get | 28.0 µs | 47.3 µs |

Memory, 20,000 connections that each made one request and then sat idle (process RSS growth):

| Server | Per connection |
|---|---|
| bulkapi `uring` | 4.33 KB |
| bulkapi `uring-ms` | 4.54 KB |
| reference-nodb | 0 (its connection table is faulted in at startup) |

## What the numbers say

- **Few connections: bulkapi, by 9x.** The reference waits for 64 completions or 2 ms before each batch.
  With 16 connections it never gets 64, so every request pays the 2 ms; its CPU is mostly idle.
- **Pipelining: bulkapi, by 6x to 15x on one thread.** The reference answers one request per connection per
  batch; bulkapi answers everything buffered in one pass.
- **Requests larger than the receive buffer: bulkapi, by 9x at the default.** A 4.5 KB bulk insert spans five
  1 KB buffers and the reference reads one per batch. With 16 KB buffers the two are equal.
- **Many connections, small requests: the reference, by about 1.9x per core** at 2,000 connections. At 256
  the two cost the same CPU per request.
- **Bulk get: the reference, by 1.7x per core.** Its store has no locks, its JSON escaping scans 16 bytes
  at a time, and the response body is written once. These are the first four items on bulkapi's
  optimisation list in `../COMPARISON.md`.
- **Idle memory: the reference.** bulkapi's 16 KB receive buffer costs about 4.3 KB resident per connection
  once used; the reference's connections cost nothing beyond its preallocated table.
- **More cores: bulkapi.** The reference is one thread by design; bulkapi with four threads is ahead on
  every workload here.

A later same-session rerun against bulkapi's reworked `uring-ms` backend (no per-connection buffer),
all single-threaded, requests per second:

| Workload | reference-nodb | bulkapi `uring` | bulkapi `uring-ms` |
|---|---|---|---|
| get, 16 conns | 6.5k | 74k | 51-72k |
| get, 256 conns | 84k | 82k | 63k |
| get, 2,000 conns | 62k | 30k | 46k |
| get, pipeline 8 | 66k | 390-640k | 410-440k |
| bulk get, 100 items | 36k | 22k | 13-19k |

`uring-ms` closes part of the gap at 2,000 connections (20-25 us of CPU per request in this run against
the reference's 14-15); the reference is still ahead there and on bulk get.

Run-to-run variation on this machine is large (bulkapi x1 on the 256-connection get ranged from 52k to
97k req/s across runs), so differences under about 1.5x should not be read as real.
