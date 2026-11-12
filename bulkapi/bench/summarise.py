#!/usr/bin/env python3
"""Median per (workload, server) from run_bench.py output, as a markdown table."""
import json, statistics, sys
from collections import defaultdict

rows = [json.loads(l) for l in open(sys.argv[1]) if l.strip()]
ALL = ["ref", "base-uring", "new-uring", "base-ms", "new-ms", "java"]
order = []
d = defaultdict(list)
for r in rows:
    if r["workload"] not in order: order.append(r["workload"])
    d[(r["workload"], r["server"])].append(r)
servers = [s for s in ALL if any(r["server"] == s for r in rows)]
med = lambda xs: statistics.median(xs) if xs else float("nan")

def table(key, fmt, title):
    print(f"\n{title}\n")
    print("| Workload | " + " | ".join(servers) + " | best new vs ref |")
    print("|---|" + "---|" * (len(servers) + 1))
    for w in order:
        vals = {s: med([r[key] for r in d[(w, s)] if r.get(key) is not None]) for s in servers}
        if key in ("us_per_req", "p50_us"):
            best = min(vals["new-uring"], vals["new-ms"]); ratio = vals["ref"] / best
        else:
            best = max(vals["new-uring"], vals["new-ms"]); ratio = best / vals["ref"]
        print(f"| {w} | " + " | ".join(fmt(vals[s]) for s in servers) + f" | {ratio:.2f}x |")

table("us_per_req", lambda v: f"{v:.2f}", "Server CPU per request (us), median")
table("rps", lambda v: f"{v:,.0f}", "Throughput (req/s), median")
table("p50_us", lambda v: f"{v:,.0f}", "p50 latency (us), median; last column = ref p50 / best new p50")
n = defaultdict(int); errs = 0
for r in rows: n[r["server"]] += 1; errs += r.get("errors", 0)
spread = []
for (w, s), rs in d.items():
    v = [r["us_per_req"] for r in rs]
    if len(v) > 1: spread.append((max(v) / min(v), w, s))
spread.sort(reverse=True)
print(f"\nruns per server: {dict(n)}; total errors: {errs}")
print("widest spread (max/min us/req): " + ", ".join(f"{w} {s} {x:.2f}x" for x, w, s in spread[:4]))
