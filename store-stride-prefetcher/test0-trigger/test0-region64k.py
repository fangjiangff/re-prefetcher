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
SRC = os.path.join(BASE_DIR, "test0-region64k.c")
UTIL_SRC = os.path.join(ROOT_DIR, "until.c")
OUT = os.path.join(ROOT_DIR, "bin", "test0-region64k")
RESULT_DIR = os.path.join(ROOT_DIR, "res", "store-stride-region64k")


def parse_args():
    parser = argparse.ArgumentParser(
        description=(
            "Test whether a store-stride entry survives a 4 KiB boundary "
            "inside one 64 KiB physical region while requests remain page bounded."
        )
    )
    parser.add_argument("--arch", choices=ARCH_CONFIG.keys(), default="X925")
    parser.add_argument("--core", type=int, default=None)
    parser.add_argument("--stride", type=int, default=5,
                        help="Stride in cache lines. Default: 5")
    parser.add_argument("--train-accesses", type=int, default=5,
                        help="Stores before the 4 KiB boundary. Default: 5")
    parser.add_argument("--rounds", type=int, default=40000)
    parser.add_argument("--buddy-pages", type=int, default=16384,
                        help="Candidate 4 KiB pages used to find a contiguous run.")
    parser.add_argument("--hit-threshold", type=int, default=None)
    parser.add_argument("--timer", choices=["gettime", "cntvct", "pmccntr"],
                        default="cntvct")
    parser.add_argument("--cc", default=os.environ.get("CC", "gcc"))
    parser.add_argument("--objdump", default=os.environ.get("OBJDUMP", "objdump"))
    parser.add_argument("--no-dump", action="store_true")
    args = parser.parse_args()

    if args.core is None:
        args.core = ARCH_CONFIG[args.arch]["core"]
    if args.hit_threshold is None:
        args.hit_threshold = ARCH_CONFIG[args.arch]["threshold_ns"]
    if args.core < 0 or args.stride < 1 or args.train_accesses < 2:
        parser.error("core/stride must be positive and train-accesses must be >= 2")
    if args.rounds < 1 or args.buddy_pages < 17:
        parser.error("rounds must be positive and buddy-pages must be >= 17")
    if args.train_accesses * args.stride >= 64:
        parser.error("train-accesses * stride must be < 64 cache lines")
    return args


def timer_define(timer):
    return {
        "gettime": "-DGETTIME=1",
        "cntvct": "-DCNTVCT=1",
        "pmccntr": "-DPMCCNTR=1",
    }[timer]


def timer_unit(timer):
    return {"gettime": "ns", "cntvct": "ticks", "pmccntr": "cycles"}[timer]


def compile_test(args):
    os.makedirs(os.path.dirname(OUT), exist_ok=True)
    cmd = [
        args.cc, "-std=gnu11", "-O0", "-static", "-march=armv8.5-a+predres",
        f"-DSTRIDE_LINES={args.stride}",
        f"-DTRAIN_ACCESSES={args.train_accesses}",
        f"-DROUNDS={args.rounds}",
        f"-DBUDDY_PAGES={args.buddy_pages}",
        timer_define(args.timer),
        "-o", OUT, SRC, UTIL_SRC,
    ]
    print("Compile:", " ".join(cmd))
    return subprocess.run(cmd).returncode


def parse_output(output):
    rows = []
    for line in output.splitlines():
        if not line or line.startswith("#") or line.startswith("case"):
            continue
        fields = line.split()
        if len(fields) != 2:
            continue
        try:
            rows.append({"case": fields[0], "latency": int(fields[1])})
        except ValueError:
            pass
    return rows


def classify(rows, threshold):
    return {
        row["case"]: row["latency"] <= threshold
        for row in rows
    }


def print_summary(rows, threshold, unit):
    hits = classify(rows, threshold)
    print(f"\nThreshold: {threshold} {unit}")
    for row in rows:
        state = "hit/prefetched" if hits[row["case"]] else "miss"
        print(f"{row['case']:38s} {row['latency']:8d} {unit}  {state}")

    required = {
        "same_page_baseline",
        "intra64k_p7_to_p8_cross_request",
        "intra64k_p7_to_p8_resume",
        "intra64k_p7_to_p8_trigger_only",
        "inter64k_p15_to_p0_cross_request",
        "inter64k_p15_to_p0_resume",
        "inter64k_p15_to_p0_trigger_only",
    }
    if not required.issubset(hits):
        return

    supports = (
        hits["same_page_baseline"]
        and not hits["intra64k_p7_to_p8_cross_request"]
        and hits["intra64k_p7_to_p8_resume"]
        and not hits["intra64k_p7_to_p8_trigger_only"]
        and not hits["inter64k_p15_to_p0_cross_request"]
        and not hits["inter64k_p15_to_p0_resume"]
        and not hits["inter64k_p15_to_p0_trigger_only"]
    )
    print("\nHypothesis supported by this run:", "yes" if supports else "no")


def main():
    args = parse_args()
    if compile_test(args) != 0:
        return 1

    os.makedirs(RESULT_DIR, exist_ok=True)
    stem = (f"{args.arch}-core{args.core}-stride{args.stride}-"
            f"accesses{args.train_accesses}-{args.timer}")
    if not args.no_dump:
        dump = subprocess.run([args.objdump, "-d", OUT], capture_output=True, text=True)
        if dump.returncode == 0:
            with open(os.path.join(RESULT_DIR, stem + ".dump"), "w") as f:
                f.write(dump.stdout)

    run = subprocess.run(
        ["taskset", "-c", str(args.core), OUT],
        capture_output=True,
        text=True,
    )
    if run.stdout:
        print(run.stdout, end="")
    if run.returncode != 0:
        if run.stderr:
            print(run.stderr, file=sys.stderr, end="")
        if run.returncode == 2:
            print("Hint: PFNs usually require sudo/CAP_SYS_ADMIN.", file=sys.stderr)
        return run.returncode

    rows = parse_output(run.stdout)
    if not rows:
        print("No result rows were produced.", file=sys.stderr)
        return 1

    raw_path = os.path.join(RESULT_DIR, stem + ".txt")
    tsv_path = os.path.join(RESULT_DIR, stem + ".tsv")
    with open(raw_path, "w") as f:
        f.write(run.stdout)
    with open(tsv_path, "w", newline="") as f:
        writer = csv.DictWriter(f, fieldnames=["case", "latency"], delimiter="\t")
        writer.writeheader()
        writer.writerows(rows)

    print_summary(rows, args.hit_threshold, timer_unit(args.timer))
    print(f"\nSaved raw output to {raw_path}")
    print(f"Saved TSV to {tsv_path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
