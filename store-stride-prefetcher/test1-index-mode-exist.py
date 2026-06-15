import argparse
import os
import subprocess
import sys

from cross_test_config import ARCH_CONFIG, arch_choices


BASE_DIR = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.join(BASE_DIR, "test1-index-mode-exist.c")
UTIL_SRC = os.path.join(BASE_DIR, "until.c")
OUT = os.path.join(BASE_DIR, "bin", "test1-index-mode-exist")

DEFAULT_STRIDE_LINES = 5
DEFAULT_TRAIN_STEP = None
DEFAULT_ROUNDS = 40000
DEFAULT_PROBE_POSITIONS = 100
DEFAULT_REPEAT = 5
GREEN = "\033[32m"
RED = "\033[31m"
RESET = "\033[0m"

SUMMARY_LABELS = {
    "T0_no_trigger": "T0_no_trigger (sanity without trigger)",
    "T0": "T0 (same PC/VA/PA, noinline trigger)",
    "T1": "T1 (different PC, same VA/PA)",
    "T2": "T2 (same process, different VA, same PA)",
    "T3": "T3 (cross-process, same VA, different PA)",
}


def parse_args():
    parser = argparse.ArgumentParser(
        description="Store-stride index-mode test based on test0-exist."
    )
    parser.add_argument("--arch", required=True, choices=arch_choices())
    parser.add_argument("--core", type=int, default=None,
                        help="Override CPU core. Default comes from --arch.")
    parser.add_argument("--stride", type=int, default=DEFAULT_STRIDE_LINES,
                        help="Stride in cache lines. Default: 5")
    parser.add_argument("--train-step", type=int, default=DEFAULT_TRAIN_STEP,
                        help="Override test0-style train_step. Default is arch store train_accesses + 1.")
    parser.add_argument("--rounds", type=int, default=DEFAULT_ROUNDS)
    parser.add_argument("--probe-positions", type=int,
                        default=DEFAULT_PROBE_POSITIONS)
    parser.add_argument("--repeat", type=int, default=DEFAULT_REPEAT,
                        help="Repeat train+trigger sequence inside each round. Default matches test0-exist.")
    parser.add_argument("--hit-threshold-ns", dest="threshold_ns", type=int,
                        default=None,
                        help="avg_ns <= threshold is classified as yes. Default comes from --arch.")
    parser.add_argument("--cc", default=os.environ.get("CC", "gcc"))
    parser.add_argument("--no-compile", action="store_true")
    parser.add_argument("--verbose", action="store_true",
                        help="Print raw C output to stderr before summary.")
    args = parser.parse_args()

    if args.core is None:
        args.core = ARCH_CONFIG[args.arch]["core"]
    if args.train_step is None:
        args.train_step = ARCH_CONFIG[args.arch]["train_accesses"]["store"] + 1
    if args.threshold_ns is None:
        args.threshold_ns = ARCH_CONFIG[args.arch]["threshold_ns"]

    if args.core < 0:
        parser.error("--core must be >= 0")
    if args.stride < 1:
        parser.error("--stride must be >= 1")
    if args.train_step < 2:
        parser.error("--train-step must be >= 2")
    if args.rounds < 1:
        parser.error("--rounds must be >= 1")
    if args.probe_positions < 1:
        parser.error("--probe-positions must be >= 1")
    if args.repeat < 1:
        parser.error("--repeat must be >= 1")
    if args.train_step * args.stride >= args.probe_positions:
        parser.error("predicted position train_step * stride must be inside probe positions")
    if args.threshold_ns < 1:
        parser.error("--hit-threshold-ns must be >= 1")
    return args


def ensure_parent(path):
    parent = os.path.dirname(path)
    if parent:
        os.makedirs(parent, exist_ok=True)


def compile_test(args):
    ensure_parent(OUT)
    cmd = [
        args.cc,
        "-std=gnu11",
        "-O0",
        "-static",
        f"-DSTRIDE_BYTES={args.stride * 64}",
        f"-DTRAIN_STEP={args.train_step}",
        f"-DROUNDS={args.rounds}",
        f"-DPROBE_POSITIONS={args.probe_positions}",
        f"-DREPEAT={args.repeat}",
        f"-DCPU_ID={args.core}",
        "-o",
        OUT,
        SRC,
        UTIL_SRC,
    ]
    return subprocess.run(cmd)


def run_test():
    return subprocess.run([OUT], capture_output=True, text=True)


def parse_results(output):
    rows = {}
    for line in output.splitlines():
        stripped = line.strip()
        if not stripped or stripped.startswith("#"):
            continue
        fields = stripped.split()
        if fields[0] == "id":
            continue
        if len(fields) != 6:
            raise ValueError(f"unexpected output row: {line}")
        rows[fields[0]] = int(fields[5])

    required = ("T0_no_trigger", "T0", "T1", "T2", "T3")
    missing = [test_id for test_id in required if test_id not in rows]
    if missing:
        raise ValueError(f"missing result rows: {', '.join(missing)}")
    return rows


def is_prefetched(rows, test_id, threshold_ns):
    return rows[test_id] <= threshold_ns


def print_summary(rows, threshold_ns):
    for test_id in ("T0_no_trigger", "T0", "T1", "T2", "T3"):
        prefetched = is_prefetched(rows, test_id, threshold_ns)
        verdict = "yes" if prefetched else "no"
        color = GREEN if prefetched else RED
        print(f"{SUMMARY_LABELS[test_id]} {color}{verdict}{RESET} avg_ns={rows[test_id]}")

    t0_ok = is_prefetched(rows, "T0", threshold_ns) and not is_prefetched(rows, "T0_no_trigger", threshold_ns)
    t1_yes = is_prefetched(rows, "T1", threshold_ns)
    t2_yes = is_prefetched(rows, "T2", threshold_ns)
    t3_yes = is_prefetched(rows, "T3", threshold_ns)

    if not t0_ok:
        print("Interpretation: T0 sanity failed, so T1/T2/T3 should not be used to infer index mode yet.", file=sys.stderr)
        print("Expected T0_no_trigger no and T0 yes.", file=sys.stderr)
        return

    print("Interpretation:")
    if t1_yes:
        print("  T1 yes: changing only the trigger PC still works; PC is not a required match condition in this setup.")
    else:
        print("  T1 no: changing only the trigger PC breaks the effect; PC may participate, or inline/noinline differs in another relevant way.")

    if t2_yes:
        print("  T2 yes: different VA with the same PA still works; identical VA is not required, and PA/alias-derived information may be sufficient.")
    else:
        print("  T2 no: different VA with the same PA does not work; this supports VA involvement, but alias/TLB/cache-maintenance effects remain possible.")

    if t3_yes:
        print("  T3 yes: same VA with different PA works across processes; this supports VA-based or context-insensitive matching, despite the cross-process caveat.")
    else:
        print("  T3 no: same VA with different PA did not work across processes; do not treat this as proof against VA indexing because context switch/ASID/state flush may be the cause.")

    if t2_yes:
        conclusion = "PA"
    elif not t1_yes:
        conclusion = "PC"
    else:
        conclusion = "VA"
    print(f"Final index guess: {GREEN}{conclusion}{RESET}")


def main():
    args = parse_args()

    if not args.no_compile:
        compile_result = compile_test(args)
        if compile_result.returncode != 0:
            print("Compile failed", file=sys.stderr)
            return compile_result.returncode

    run_result = run_test()
    if run_result.stderr:
        print(run_result.stderr, end="", file=sys.stderr)
    if run_result.returncode != 0:
        if run_result.stdout:
            print(run_result.stdout, end="", file=sys.stderr)
        print("Execution failed", file=sys.stderr)
        return run_result.returncode

    if args.verbose:
        print(run_result.stdout, end="", file=sys.stderr)

    try:
        rows = parse_results(run_result.stdout)
    except ValueError as exc:
        print(f"Parse failed: {exc}", file=sys.stderr)
        return 1

    print_summary(rows, args.threshold_ns)
    return 0


if __name__ == "__main__":
    sys.exit(main())
