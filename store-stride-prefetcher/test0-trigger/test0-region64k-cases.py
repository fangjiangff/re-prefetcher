import argparse
import csv
import os
import subprocess
import sys

ROOT_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
if ROOT_DIR not in sys.path:
    sys.path.insert(0, ROOT_DIR)

from cross_test_config import ARCH_CONFIG

BASE_DIR = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.join(BASE_DIR, "test0-region64k-cases.c")
UTIL_SRC = os.path.join(ROOT_DIR, "until.c")
OUT = os.path.join(ROOT_DIR, "bin", "test0-region64k-cases")
RESULT_DIR = os.path.join(ROOT_DIR, "res", "store-stride-region64k")


def parse_args():
    p = argparse.ArgumentParser(description="Four-page 64KB-region store-stride cases")
    p.add_argument("--arch", choices=ARCH_CONFIG.keys(), default="X925")
    p.add_argument("--core", type=int, default=None)
    p.add_argument("--rounds", type=int, default=40000)
    p.add_argument("--buddy-pages", type=int, default=16384)
    p.add_argument("--hit-threshold", type=int, default=None)
    p.add_argument("--timer", choices=["gettime", "cntvct", "pmccntr"], default="cntvct")
    p.add_argument("--cc", default=os.environ.get("CC", "gcc"))
    p.add_argument("--objdump", default=os.environ.get("OBJDUMP", "objdump"))
    p.add_argument("--no-dump", action="store_true")
    args = p.parse_args()
    if args.core is None:
        args.core = ARCH_CONFIG[args.arch]["core"]
    if args.hit_threshold is None:
        args.hit_threshold = ARCH_CONFIG[args.arch]["threshold_ns"]
    if args.rounds < 1 or args.buddy_pages < 17:
        p.error("rounds must be positive and buddy-pages must be >= 17")
    return args


def compile_test(args):
    os.makedirs(os.path.dirname(OUT), exist_ok=True)
    timer = {"gettime": "-DGETTIME=1", "cntvct": "-DCNTVCT=1", "pmccntr": "-DPMCCNTR=1"}[args.timer]
    cmd = [args.cc, "-std=gnu11", "-O0", "-static", "-march=armv8.5-a+predres",
           "-DSTRIDE_LINES=5", "-DTRAIN_ACCESSES=4", f"-DROUNDS={args.rounds}",
           f"-DBUDDY_PAGES={args.buddy_pages}", timer, "-o", OUT, SRC, UTIL_SRC]
    print("Compile:", " ".join(cmd))
    return subprocess.run(cmd).returncode


def main():
    args = parse_args()
    if compile_test(args) != 0:
        return 1
    os.makedirs(RESULT_DIR, exist_ok=True)
    stem = f"{args.arch}-core{args.core}-four-page-{args.timer}"
    if not args.no_dump:
        dump = subprocess.run([args.objdump, "-d", OUT], capture_output=True, text=True)
        if dump.returncode == 0:
            with open(os.path.join(RESULT_DIR, stem + ".dump"), "w") as f:
                f.write(dump.stdout)
    run = subprocess.run(["taskset", "-c", str(args.core), OUT], capture_output=True, text=True)
    if run.stdout:
        print(run.stdout, end="")
    if run.returncode != 0:
        if run.stderr:
            print(run.stderr, file=sys.stderr, end="")
        return run.returncode
    rows = []
    for line in run.stdout.splitlines():
        fields = line.split()
        if len(fields) == 2 and fields[0].startswith("case"):
            rows.append({"case": fields[0], "latency": int(fields[1])})
    if len(rows) != 6:
        print("Expected six case results.", file=sys.stderr)
        return 1
    unit = "ticks" if args.timer == "cntvct" else "cycles" if args.timer == "pmccntr" else "ns"
    print(f"\nThreshold: {args.hit_threshold} {unit}")
    for row in rows:
        state = "hit/prefetched" if row["latency"] <= args.hit_threshold else "miss"
        print(f"{row['case']:35s} {row['latency']:8d} {unit}  {state}")
    hits = {r["case"]: r["latency"] <= args.hit_threshold for r in rows}
    expected = (hits["case0_same_page"] and not hits["case1_cross_4k_request"] and
                hits["case2_same_64k"] and not hits["case3_same_64k_no_trainer"] and
                not hits["case4_cross_64k"] and not hits["case5_cross_64k_no_trainer"])
    print("\nHypothesis supported by this run:", "yes" if expected else "no")
    with open(os.path.join(RESULT_DIR, stem + ".txt"), "w") as f:
        f.write(run.stdout)
    with open(os.path.join(RESULT_DIR, stem + ".tsv"), "w", newline="") as f:
        writer = csv.DictWriter(f, fieldnames=["case", "latency"], delimiter="\t")
        writer.writeheader()
        writer.writerows(rows)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
