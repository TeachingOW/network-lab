#!/usr/bin/env python3
"""Compare the local store with a Git revision, without a Linux server or liburing.

    python3 bench/compare_store.py --baseline HEAD --rounds 7 --cxx clang++

Numbers cover store operations only, not network throughput. Uses identical sources
and flags for both builds, alternating their order on every round.
"""
import argparse
import os
from pathlib import Path
import statistics
import subprocess
import tempfile


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--baseline", default="HEAD")
    ap.add_argument("--rounds", type=int, default=7)
    ap.add_argument("--cxx", default=os.environ.get("CXX", "c++"))
    args = ap.parse_args()
    if args.rounds < 1:
        ap.error("--rounds must be positive")
    root = Path(__file__).resolve().parents[1]
    modes = ("overwrite", "growing", "update", "heap-update", "lookup", "insert")
    samples = {(version, mode): [] for version in ("before", "after") for mode in modes}
    with tempfile.TemporaryDirectory(prefix="bulkapi-store-") as tmp:
        tmp = Path(tmp)
        baseline = subprocess.check_output(
            ["git", "show", f"{args.baseline}:bulkapi/src/store.h"], cwd=root
        )
        (tmp / "store.h").write_bytes(baseline)
        subprocess.run(["python3", str(root / "tools/gen_routes.py"),
                        str(root / "api.spec"), str(tmp / "routes_gen.h")], check=True)
        for version, include in (("before", tmp), ("after", root / "src")):
            subprocess.run([args.cxx, "-O3", "-std=c++20", "-pthread", f"-I{include}",
                            f"-I{root / 'src'}", f"-I{tmp}", str(root / "bench/store.cpp"),
                            "-o", str(tmp / version)], check=True)
        for rnd in range(args.rounds):
            for mode in modes:
                order = ("before", "after") if rnd % 2 == 0 else ("after", "before")
                checksums = []
                for version in order:
                    row = subprocess.check_output([str(tmp / version), mode], text=True).split()
                    samples[version, mode].append(tuple(map(float, row[1:4])))
                    checksums.append(row[4])
                if checksums[0] != checksums[1]:
                    raise RuntimeError(f"different results for {mode}: {checksums}")
    print(f"Baseline: {args.baseline}; {args.rounds} alternating rounds; {args.cxx} -O3 -std=c++20")
    print("workload    before ns/op  after ns/op  before/after  allocations before/after  table bytes before/after")
    for mode in modes:
        before, after = [tuple(statistics.median(v[i] for v in samples[version, mode])
                               for i in range(3)) for version in ("before", "after")]
        print(f"{mode:10} {before[0]:12.3f} {after[0]:12.3f} {before[0]/after[0]:12.2f}x"
              f" {int(before[1]):10}/{int(after[1]):<10} {int(before[2]):12}/{int(after[2])}")


if __name__ == "__main__":
    main()
