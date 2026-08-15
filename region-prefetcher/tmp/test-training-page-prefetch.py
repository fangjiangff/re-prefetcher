#!/usr/bin/env python3

import argparse
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parent.parent
STORE_STRIDE = REPO / "store-stride-prefetcher"
sys.path.insert(0, str(STORE_STRIDE))

from cross_test_config import ARCH_CONFIG, default_timer_for_arch, timer_define_for_arch


def main():
    parser = argparse.ArgumentParser(
        description="Probe prefetches on the page used for footprint training."
    )
    parser.add_argument("--arch", choices=("A725", "X925"), default="A725")
    parser.add_argument("--core", type=int, default=None)
    parser.add_argument("--timer", choices=("gettime", "cntvct", "pmccntr"), default=None)
    parser.add_argument("--rounds", type=int, default=4096)
    parser.add_argument("--threshold", type=int, default=100)
    parser.add_argument("--wait-nops", type=int, default=1000)
    parser.add_argument("--wait-ns", type=int, default=10000)
    parser.add_argument("--output", default="/tmp/test-training-page-prefetch.txt")
    args = parser.parse_args()

    if args.core is None:
        args.core = ARCH_CONFIG[args.arch]["core"]
    if args.timer is None:
        args.timer = default_timer_for_arch(args.arch)

    binary = Path("/tmp/test-training-page-prefetch")
    source = HERE / "test-training-page-prefetch.c"
    build = [
        "gcc", "-O2", "-Wall", "-Wextra", "-march=armv8.5-a+predres",
        f"-DROUNDS={args.rounds}",
        f"-DCACHE_HIT_THRESHOLD_TICKS={args.threshold}",
        f"-DPREFETCH_WAIT_NOPS={args.wait_nops}",
        f"-DPREFETCH_WAIT_NS={args.wait_ns}",
        timer_define_for_arch(args.arch, args.timer),
        f"-I{STORE_STRIDE}", str(source), str(STORE_STRIDE / "until.c"),
        "-o", str(binary),
    ]
    print("# build:", " ".join(build))
    subprocess.run(build, check=True)

    run = ["taskset", "-c", str(args.core), str(binary)]
    print("# run:", " ".join(run))
    completed = subprocess.run(run, text=True, stdout=subprocess.PIPE,
                               stderr=subprocess.STDOUT)
    print(completed.stdout, end="")
    Path(args.output).write_text(completed.stdout)
    print(f"# output: {args.output}")
    return completed.returncode


if __name__ == "__main__":
    raise SystemExit(main())
