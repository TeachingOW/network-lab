#!/usr/bin/env python3
"""Alternating benchmark: each server pinned to one CPU, the load generator on others.

Measures throughput and latency (from bench) and the server's CPU time per request, read from
/proc/<pid>/stat over the measured window. Writes one JSON line per run.

  bench/compare.py ROOT OUT.jsonl --rounds 5 --secs 4 --server-cpu 0 --bench-cpus 2-7 --bench-threads 4
  bench/summarise.py OUT.jsonl
ROOT holds bulkapi/, reference-nodb/ and base/bulkapi/ (the version to compare against), each built:
  mkdir -p base && git archive <commit> bulkapi | tar -x -C base && make -C base/bulkapi
"""
import argparse, json, os, resource, signal, socket, subprocess, sys, time

ap = argparse.ArgumentParser()
ap.add_argument("root"); ap.add_argument("out")
ap.add_argument("--rounds", type=int, default=3); ap.add_argument("--secs", type=int, default=4)
ap.add_argument("--server-cpu", default="0"); ap.add_argument("--bench-cpus", default="2-7")
ap.add_argument("--bench-threads", type=int, default=4)
ap.add_argument("--only", default="", help="comma-separated workload names")
ap.add_argument("--servers", default="ref,base-uring,new-uring,base-ms,new-ms")
a = ap.parse_args()
R = a.root
soft, hard = resource.getrlimit(resource.RLIMIT_NOFILE)
resource.setrlimit(resource.RLIMIT_NOFILE, (min(hard, 1 << 20), hard))

SERVERS = {
    "ref":        (["{R}/reference-nodb/build/server"], {"HOST": "127.0.0.1", "PORT": "{port}"}),
    "base-uring": (["{R}/base/bulkapi/build/server", "-p", "{port}", "-t", "1", "--backend", "uring"], {}),
    "new-uring":  (["{R}/bulkapi/build/server", "-p", "{port}", "-t", "1", "--backend", "uring"], {}),
    "base-ms":    (["{R}/base/bulkapi/build/server", "-p", "{port}", "-t", "1", "--backend", "uring-ms"], {}),
    "new-ms":     (["{R}/bulkapi/build/server", "-p", "{port}", "-t", "1", "--backend", "uring-ms"], {}),
    "java":       (["java", "-Xms1g", "-Xmx2g", "-XX:+UseParallelGC", "-Djava.library.path={R}/bulkapi-java/build",
                    "-cp", "{R}/bulkapi-java/build/classes", "bulkapi.Server", "-p", "{port}", "-t", "1",
                    "--backend", "uring", "--spec", "{R}/bulkapi/api.spec"], {}),
}
WORKLOADS = [  # name, bench args
    ("get-c16", ["-m", "get", "-c", "16"]),
    ("get-c256", ["-m", "get", "-c", "256"]),
    ("get-c2000", ["-m", "get", "-c", "2000"]),
    ("get-c256-P8", ["-m", "get", "-c", "256", "-P", "8"]),
    ("put-c256-P8", ["-m", "put", "-c", "256", "-P", "8"]),
    ("bulk-n100", ["-m", "bulk", "-c", "64", "-n", "100"]),
    ("bulkget-n100", ["-m", "bulkget", "-c", "64", "-n", "100"]),
]
if a.only: WORKLOADS = [w for w in WORKLOADS if w[0] in a.only.split(",")]
servers = a.servers.split(",")
BENCH = f"{R}/bulkapi/build/bench"
TICK = os.sysconf("SC_CLK_TCK")

def cpu_s(pid):
    f = open(f"/proc/{pid}/stat").read().rsplit(")", 1)[1].split()
    return (int(f[11]) + int(f[12])) / TICK  # utime + stime

def wait_port(port, t=5):
    end = time.time() + t
    while time.time() < end:
        try: socket.create_connection(("127.0.0.1", port), 0.2).close(); return True
        except OSError: time.sleep(0.05)
    return False

def parse(out):
    r = {}
    for line in out.splitlines():
        if line.startswith("throughput:"): r["rps"] = float(line.split()[1])
        elif line.startswith("errors:"): r["errors"] = int(line.split()[1])
        elif line.startswith("latency"):
            p = line.split(":", 1)[1].split()
            d = dict(zip(p[0::2], p[1::2])); r["p50_us"] = float(d["p50"]); r["p99_us"] = float(d["p99"])
    return r

port = 20000 + os.getpid() % 10000
outf = open(a.out, "a")
for rnd in range(a.rounds):
    for wname, wargs in WORKLOADS:
        order = servers[rnd % len(servers):] + servers[:rnd % len(servers)]
        for sname in order:
            port += 1
            cmd, env = SERVERS[sname]
            cmd = [c.format(R=R, port=port) for c in cmd]
            e = dict(os.environ, **{k: v.format(port=port) for k, v in env.items()})
            srv = subprocess.Popen(["taskset", "-c", a.server_cpu] + cmd, env=e,
                                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            if not wait_port(port):
                print(f"{sname} did not start", file=sys.stderr); srv.kill(); continue
            pin = ["taskset", "-c", a.bench_cpus, BENCH, "-p", str(port)]
            if wname.startswith(("get", "bulkget")):
                subprocess.run(pin + ["-m", "get", "-c", "1", "-d", "1", "-w", "0"], capture_output=True)
            b = subprocess.Popen(pin + wargs + ["-t", str(min(a.bench_threads, int(wargs[3]))), "-d", str(a.secs),
                                 "-w", "1", "--no-preload"], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
            t0 = time.time()
            time.sleep(1.15); c0, w0 = cpu_s(srv.pid), time.time()
            time.sleep(a.secs - 0.3); c1, w1 = cpu_s(srv.pid), time.time()
            out = b.communicate()[0]
            srv.send_signal(signal.SIGTERM)
            try: srv.wait(3)
            except subprocess.TimeoutExpired: srv.kill(); srv.wait()
            r = parse(out)
            if "rps" not in r:
                print(f"{sname} {wname}: bench failed:\n{out}", file=sys.stderr); continue
            util = (c1 - c0) / (w1 - w0)
            r.update(round=rnd, workload=wname, server=sname, cpu_util=round(util, 3),
                     us_per_req=round(util / r["rps"] * 1e6, 2) if r["rps"] else None)
            outf.write(json.dumps(r) + "\n"); outf.flush()
            print(f"r{rnd} {wname:13} {sname:10} {r['rps']:>11,.0f} req/s  cpu {util*100:5.1f}%  "
                  f"{r['us_per_req']:6.2f} us/req  p50 {r['p50_us']:7.0f}  p99 {r['p99_us']:7.0f}  err {r['errors']}", flush=True)
