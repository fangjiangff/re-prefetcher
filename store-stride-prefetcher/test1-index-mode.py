import argparse
import os
import subprocess
import sys

from cross_test_config import (
    apply_single_core_defaults,
    apply_threshold_defaults,
    apply_train_access_defaults,
    arch_choices,
)


BASE_DIR = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.join(BASE_DIR, "test1-index-mode.c")
UTIL_SRC = os.path.join(BASE_DIR, "until.c")
OUT = os.path.join(BASE_DIR, "bin", "test1-index-mode")

DEFAULT_STRIDE_LINES = 5
DEFAULT_ROUNDS = 2000
DEFAULT_TRAIN_PC = "0x500000120"
DEFAULT_TRIGGER_PC = "0x7000009a0"
GREEN = "\033[32m"
RED = "\033[31m"
RESET = "\033[0m"

SUMMARY_LABELS = {
    "T0_no_trigger": "T0_no_trigger (sanity without trigger)",
    "T0_trigger": "T0_trigger (sanity with trigger)",
    "T1": "T1 (PC index?)",
    "T2": "T2 (VA index?)",
    "T3": "T3 (PA index?)",
    "T4": "T4 (VA&PA index?)",
    "T5": "T5 (negative: different PC/VA/PA)",
}


def parse_int_auto(value):
    try:
        return int(value, 0)
    except ValueError as exc:
        raise argparse.ArgumentTypeError(str(exc)) from exc


def parse_args():
    parser = argparse.ArgumentParser(
        description="Compile and classify store-stride prefetcher index mode."
    )
    parser.add_argument("--arch", required=True, choices=arch_choices())
    parser.add_argument("--core", type=int, default=None,
                        help="Override CPU core. Default comes from --arch.")
    parser.add_argument("--stride", type=int, default=DEFAULT_STRIDE_LINES,
                        help="Stride in cache lines. Default: 5")
    parser.add_argument("--train-accesses", type=int, default=None,
                        help="Override store training accesses. Default comes "
                             "from --arch store config.")
    parser.add_argument("--rounds", type=int, default=DEFAULT_ROUNDS)
    parser.add_argument("--hit-threshold-ns", dest="threshold_ns", type=int,
                        default=None,
                        help="avg_ns <= threshold is classified as yes. "
                             "Default comes from --arch.")
    parser.add_argument("--train-pc", type=parse_int_auto,
                        default=parse_int_auto(DEFAULT_TRAIN_PC))
    parser.add_argument("--trigger-pc", type=parse_int_auto,
                        default=parse_int_auto(DEFAULT_TRIGGER_PC))
    parser.add_argument("--cc", default=os.environ.get("CC", "gcc"))
    parser.add_argument("--no-compile", action="store_true")
    parser.add_argument("--verbose", action="store_true",
                        help="Print raw C output to stderr before summary.")
    parser.add_argument("--raw-output", default=None,
                        help="Optional path to save raw C output.")
    args = parser.parse_args()

    args.access = "store"
    apply_single_core_defaults(args)
    apply_train_access_defaults(args)
    apply_threshold_defaults(args)

    if args.core < 0:
        parser.error("--core must be >= 0")
    if args.stride < 1:
        parser.error("--stride must be >= 1")
    if args.train_accesses < 1:
        parser.error("--train-accesses must be >= 1")
    if args.rounds < 1:
        parser.error("--rounds must be >= 1")
    if args.threshold_ns < 1:
        parser.error("--hit-threshold-ns must be >= 1")
    if (args.train_accesses + 1) * args.stride >= 128:
        parser.error("train/trigger/probe sequence must fit in two 4KB pages")
    if args.train_pc == args.trigger_pc:
        parser.error("--train-pc and --trigger-pc must be different")

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
        f"-DTRAIN_ACCESSES={args.train_accesses}",
        f"-DSTRIDE_LINES={args.stride}",
        f"-DROUNDS={args.rounds}",
        f"-DCPU_ID={args.core}",
        f"-DTRAIN_PC=0x{args.train_pc:x}ULL",
        f"-DTRIGGER_PC=0x{args.trigger_pc:x}ULL",
        "-o",
        OUT,
        SRC,
        UTIL_SRC,
    ]
    return subprocess.run(cmd)


def run_test(args):
    return subprocess.run([OUT], capture_output=True, text=True)


def save_raw(path, text):
    if path is None:
        return
    ensure_parent(path)
    with open(path, "w") as f:
        f.write(text)


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

        test_id = fields[0]
        try:
            avg_ns = int(fields[5])
        except ValueError as exc:
            raise ValueError(f"invalid latency row: {line}") from exc
        rows[test_id] = avg_ns

    required = ("T0_no_trigger", "T0_trigger", "T1", "T2", "T3", "T4", "T5")
    missing = [test_id for test_id in required if test_id not in rows]
    if missing:
        raise ValueError(f"missing result rows: {', '.join(missing)}")
    return rows


def is_prefetched(rows, test_id, threshold_ns):
    return rows[test_id] <= threshold_ns


def validate_t0(rows, threshold_ns, train_accesses):
    no_trigger_yes = is_prefetched(rows, "T0_no_trigger", threshold_ns)
    trigger_yes = is_prefetched(rows, "T0_trigger", threshold_ns)

    if trigger_yes and not no_trigger_yes:
        return True

    print("T0 sanity check failed.", file=sys.stderr)
    print(
        "Expected: T0_no_trigger no, T0_trigger yes. "
        f"Observed: T0_no_trigger {'yes' if no_trigger_yes else 'no'}, "
        f"T0_trigger {'yes' if trigger_yes else 'no'}.",
        file=sys.stderr,
    )
    print(
        "Please change --train-accesses. "
        f"Current --train-accesses={train_accesses}.",
        file=sys.stderr,
    )
    return False


def print_summary(rows, threshold_ns):
    for test_id in ("T0_no_trigger", "T0_trigger", "T1", "T2", "T3", "T4", "T5"):
        prefetched = is_prefetched(rows, test_id, threshold_ns)
        verdict = "yes" if prefetched else "no"
        color = GREEN if prefetched else RED
        print(f"{SUMMARY_LABELS[test_id]} {color}{verdict}{RESET}")


def main():
    args = parse_args()

    if not args.no_compile:
        compile_result = compile_test(args)
        if compile_result.returncode != 0:
            print("Compile failed", file=sys.stderr)
            return compile_result.returncode

    run_result = run_test(args)
    if run_result.stderr:
        print(run_result.stderr, end="", file=sys.stderr)
    if run_result.returncode != 0:
        if run_result.stdout:
            print(run_result.stdout, end="", file=sys.stderr)
        print("Execution failed", file=sys.stderr)
        return run_result.returncode

    save_raw(args.raw_output, run_result.stdout)
    if args.verbose:
        print(run_result.stdout, end="", file=sys.stderr)

    try:
        rows = parse_results(run_result.stdout)
    except ValueError as exc:
        print(f"Parse failed: {exc}", file=sys.stderr)
        return 1

    if not validate_t0(rows, args.threshold_ns, args.train_accesses):
        return 1

    print_summary(rows, args.threshold_ns)
    return 0


if __name__ == "__main__":
    sys.exit(main())
