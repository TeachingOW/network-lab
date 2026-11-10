#!/usr/bin/env python3
"""Simple asyncio load generator for bulkapi (pure stdlib).

  python3 bench/bench.py -m get -c 64 -d 10
  python3 bench/bench.py -m bulk -n 500 -c 16 -P 2

Modes: health | get | put | bulk | bulkget   (-n = items per bulk request)
Latency is per pipelined batch when -P > 1. One Python process is the limit
for what this can generate; use build/bench for peak numbers.
"""
import argparse
import asyncio
import random
import statistics
import time


def make_req(method, path, body=b""):
    head = f"{method} {path} HTTP/1.1\r\nHost: bench\r\nContent-Type: application/json\r\n"
    if body or method == "POST" or method == "PUT":
        head += f"Content-Length: {len(body)}\r\n"
    return head.encode() + b"\r\n" + body


def build_pool(a, rng):
    big = a.mode in ("bulk", "bulkget")
    pool = []
    for _ in range(64 if big else 1024):
        k = rng.randrange(a.keyspace)
        if a.mode == "health":
            pool.append(make_req("GET", "/health"))
        elif a.mode == "get":
            pool.append(make_req("GET", f"/items/{k}"))
        elif a.mode == "put":
            pool.append(make_req("PUT", f"/items/{k}", b"hello-bench-value-0123456789"))
        elif a.mode == "bulk":
            body = "".join(
                f'{{"id":{rng.randrange(a.keyspace)},"v":"value-0123456789abcdef"}}\n'
                for _ in range(a.batch)
            ).encode()
            pool.append(make_req("POST", "/items/bulk", body))
        elif a.mode == "bulkget":
            body = "".join(f"{rng.randrange(a.keyspace)}\n" for _ in range(a.batch)).encode()
            pool.append(make_req("POST", "/items/bulk/get", body))
        else:
            raise SystemExit(f"unknown mode {a.mode}")
    return pool


async def read_response(r):
    head = await r.readuntil(b"\r\n\r\n")
    status = int(head[9:12])
    cl = 0
    for line in head.split(b"\r\n")[1:]:
        if line[:15].lower() == b"content-length:":
            cl = int(line[15:])
            break
    if cl:
        await r.readexactly(cl)
    return status, len(head) + cl


async def preload(a):
    r, w = await asyncio.open_connection(a.host, a.port)
    for i in range(0, a.keyspace, 1000):
        body = "".join(
            f'{{"id":{k},"v":"value-0123456789abcdef"}}\n'
            for k in range(i, min(a.keyspace, i + 1000))
        ).encode()
        w.write(make_req("POST", "/items/bulk", body))
        status, _ = await read_response(r)
        if status != 200:
            raise SystemExit(f"preload failed: HTTP {status}")
    w.close()


async def conn_worker(a, pool, start_idx, warm_end, end, stats):
    r, w = await asyncio.open_connection(a.host, a.port)
    i = start_idx
    n = len(pool)
    while True:
        t0 = time.perf_counter()
        if t0 >= end:
            break
        w.write(b"".join(pool[(i + k) % n] for k in range(a.pipeline)))
        i += a.pipeline
        for _ in range(a.pipeline):
            status, nbytes = await read_response(r)
            if t0 >= warm_end:
                stats["reqs"] += 1
                stats["bytes"] += nbytes
                if not 200 <= status < 300:
                    stats["errs"] += 1
        if t0 >= warm_end:
            stats["lat"].append(time.perf_counter() - t0)
    w.close()


async def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("-H", "--host", default="127.0.0.1")
    ap.add_argument("-p", "--port", type=int, default=8080)
    ap.add_argument("-m", "--mode", default="get")
    ap.add_argument("-c", "--conns", type=int, default=32)
    ap.add_argument("-d", "--secs", type=int, default=10)
    ap.add_argument("-w", "--warmup", type=int, default=1)
    ap.add_argument("-P", "--pipeline", type=int, default=1)
    ap.add_argument("-n", "--batch", type=int, default=100)
    ap.add_argument("-K", "--keyspace", type=int, default=100000)
    ap.add_argument("--no-preload", action="store_true")
    a = ap.parse_args()

    if a.mode in ("get", "bulkget") and not a.no_preload:
        print(f"preloading {a.keyspace} keys...")
        await preload(a)
    pool = build_pool(a, random.Random(1))
    print(f"mode={a.mode} conns={a.conns} pipeline={a.pipeline} batch={a.batch} "
          f"duration={a.secs}s (+{a.warmup}s warmup)")

    stats = {"reqs": 0, "errs": 0, "bytes": 0, "lat": []}
    t0 = time.perf_counter()
    warm_end = t0 + a.warmup
    end = warm_end + a.secs
    await asyncio.gather(*(conn_worker(a, pool, i * 7, warm_end, end, stats) for i in range(a.conns)))

    secs = a.secs
    rps = stats["reqs"] / secs
    print(f"requests:   {stats['reqs']} in {secs}s")
    print(f"throughput: {rps:.0f} req/s" + (f"  ({rps * a.batch:.0f} items/s)" if a.mode in ("bulk", "bulkget") else ""))
    print(f"read:       {stats['bytes'] / secs / 1e6:.2f} MB/s")
    print(f"errors:     {stats['errs']}")
    lat = sorted(stats["lat"])
    if lat:
        pc = lambda p: lat[min(len(lat) - 1, int(len(lat) * p / 100))] * 1e6
        print(f"latency (us): avg {statistics.mean(lat) * 1e6:.0f}  p50 {pc(50):.0f}  "
              f"p90 {pc(90):.0f}  p99 {pc(99):.0f}  max {lat[-1] * 1e6:.0f}")
    return 1 if stats["errs"] else 0


if __name__ == "__main__":
    raise SystemExit(asyncio.run(main()))
