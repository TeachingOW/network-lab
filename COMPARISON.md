# bulkapi vs c-iouring-pavlyhalim (twelve-dollar-server-challenge PR #245)

Reference: `reference/c-iouring-pavlyhalim/` at merged commit `6a1eab9` (read-only copy, unmodified).
Files that matter: `src/server.c` (event loop), `src/sys.h` (raw io_uring ABI), `src/http.h` (parser).

## Different goals

| | bulkapi (ours) | c-iouring-pavlyhalim |
|---|---|---|
| Target | max req/s and bulk items/s on many cores | max *concurrent connections* on 1 vCPU / 2 GB (~295k held) |
| Data | in-memory sharded hash map | SQLite B-tree, durable, group commit |
| Threads | one per core, one ring each | **one thread**, up to 8 rings |
| Routing | generated from `api.spec` at build time | hand-written `if`/`memcmp` in `app.h` |
| Fallback | epoll | none (needs Linux 6.1+) |

Almost every design choice in theirs follows from "memory per idle connection is the ceiling".

## Side by side, by mechanism

### 1. Waiting for data (the biggest difference)
- **Ours:** every connection always has a single-shot `IORING_OP_RECV` armed into its own heap buffer (16 KB to start, `conn_new`/`sq_recv`). 300k idle connections would pin ~4.8 GB of user buffers, plus a pinned buffer per in-flight recv.
- **Theirs:** an idle connection only holds a **multishot `POLL_ADD`** (`POLLIN|POLLRDHUP`, `sqe_poll_conn`), no buffer, ~40 B userspace record. When it fires, the connection goes on a FIFO read queue (`queue_read`). `issue_reads()` then sends one `RECV` with `MSG_DONTWAIT | IOSQE_BUFFER_SELECT` from a shared **provided-buffer ring** (4096 x 1 KB per ring), never more than free buffers (`rs->reads >= br_entries`). Requests are parsed in place in the kernel-selected buffer; only leftovers get copied to a per-conn `pend` (rare path).
- Why: their earlier version used multishot recv; after a stall every recv fired at once, the buffer ring ran dry, `ENOBUFS` re-arm storms, OOM. The poll + bounded read queue absorbs any burst size.

### 2. File descriptors
- **Ours:** normal fds from `accept`, every op does an fd table lookup.
- **Theirs:** multishot accept with `file_index = IORING_FILE_INDEX_ALLOC` -> **direct descriptors** in a sparse registered table (`IORING_REGISTER_FILES2`), all ops use `IOSQE_FIXED_FILE`. Skips fdget/fdput per op and isn't capped by the process fd table, which is why they shard across up to 8 rings x 65,471 slots.

### 3. Op identity / stale completions
- **Ours:** `user_data = Conn* | op` in the low 3 bits. Safe only because each conn has exactly one op in flight.
- **Theirs:** `user_data = op<<56 | ring<<48 | slot<<32 | gen`. A **generation counter** per slot, bumped on free, so a late CQE for a recycled slot is dropped (`c->gen != UD_GEN(...)`). Needed because poll + recv + send + close can overlap.

### 4. Batching and syscalls
- **Ours:** `io_uring_enter(min_complete=1)` each loop; reply sent as soon as a recv is processed.
- **Theirs:** timed wait with `IORING_ENTER_EXT_ARG`: **64 completions or 2 ms when busy**, up to 1 s idle (`ring_wait`). All completions -> answers into 1 MB refcounted **arenas** -> one DB commit -> then all sends (`flush_replies`). Reads for newly-ready conns are submitted *mid-batch* and reaped again so the request is answered the same turn (p95 4.7 -> 2.7 ms in their notes). CQ head and buffer-ring tail are published every 256 CQEs during reaping so buffers return early.
- Ring 0 is `SINGLE_ISSUER | DEFER_TASKRUN`; rings 1..N are not (so the kernel posts their CQEs while the thread sleeps) and signal ring 0 via a registered **eventfd** polled multishot on ring 0. One thread drives 8 rings with one blocking wait.

### 5. Fairness / backpressure
- **Ours:** none. A connection with 1000 pipelined requests is fully processed in one go; reads are unbounded.
- **Theirs:** `READS_PER_BATCH 16384`, `ANSWERS_PER_BATCH 2048`. "Drain first": read from sockets faster than you answer, so data sits in compact user buffers instead of 3-4 KB kernel socket buffers. Also **one request per connection per batch** (`conn_feed` stops at `CF_REPLIED`) - fair, but it serialises pipelined requests. That is the right call for their workload (no pipelining clients) and the wrong one for ours.

### 6. Closing
- **Ours:** synchronous `close(fd)` from userspace once no op is in flight.
- **Theirs:** three hard-linked SQEs: `ASYNC_CANCEL` (the poll, **by user_data**, a hash lookup instead of a scan of 160k armed polls) -> `SHUTDOWN` -> `CLOSE` direct slot, with `IOSQE_CQE_SKIP_SUCCESS` so only the close posts a CQE. Required with direct descriptors: an armed op keeps the file alive after its slot closes.

### 7. Accept path
- **Ours:** one listener per worker thread (SO_REUSEPORT), multishot accept, single-shot fallback.
- **Theirs:** **8 listeners** on one thread, so 8 x somaxconn (~32k) connections can queue during a stall instead of 4k. Accepts are armed only on the ring being filled and cancelled 4096 slots before its direct table is full (a full table makes the kernel RST the client).

### 8. Memory hygiene
Theirs: `mlock` the binary's own mappings, `MADV_POPULATE_WRITE` prefault for conn tables / buffers / arenas / stack, prewarmed lists, CQ of 65,536 so nothing ever goes to the kernel overflow list (which allocates per CQE under memory pressure), lazy ring creation with `ENOMEM` -> half-size retry, idle sweep at 90 s, optional "memory valve" (off - it hurt). PGO + static build.
Ours: pooled `Conn` objects, frees oversized buffers when idle. No idle timeout, no prefaulting.

### 9. HTTP parsing
Basically the same approach in both: `memmem` for `\r\n\r\n`, length-switch on header name, case-insensitive compare, reject `Transfer-Encoding`. They answer bad input 400+close; so do we.

## What bulkapi should take, in order

1. **Poll + provided-buffer ring + bounded read queue** instead of a per-connection buffer and an always-armed recv. Biggest win for connection count and memory. Keep our "process everything buffered" path for pipelined throughput.
2. **Direct descriptors** (`FILE_INDEX_ALLOC` + `IOSQE_FIXED_FILE`) and the linked cancel/shutdown/close.
3. **Generation counters in user_data** - a prerequisite for 1 and 2 (several ops per connection).
4. **Timed batched wait** (`EXT_ARG`, min N CQEs or ~1-2 ms) and publishing CQ head / buffer tail every 256 CQEs.
5. **Idle sweep** and an accept cutoff before the table is full.
6. Optional: more listeners per thread, prefault/mlock, PGO.

## What not to take
- One answer per connection per batch - it would throw away our pipelining numbers (~1.9M vs ~200k req/s).
- Group commit / arenas held until commit - we have no durable store; send immediately.
- The memory valve - their own measurements say it made things worse.
- Single-threaded design - right for 1 vCPU, wrong for a multi-core box; per-thread rings already give us that.

## Where ours is ahead
Multi-core scaling, request pipelining, bulk NDJSON endpoints, spec-generated routing, and an epoll fallback for older kernels.
