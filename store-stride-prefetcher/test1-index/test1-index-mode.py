import argparse
import os
import re
import subprocess
import sys

ROOT_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
if ROOT_DIR not in sys.path:
    sys.path.insert(0, ROOT_DIR)

from cross_test_config import (
    ARCH_CONFIG,
    arch_choices,
    default_timer_for_arch,
    timer_define_for_arch,
    timer_unit_for_arch,
)


BASE_DIR = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.join(BASE_DIR, "test1-index-mode.c")
PMU_SRC = os.path.join(BASE_DIR, "test1-index-mode-pmu.c")
UTIL_SRC = os.path.join(ROOT_DIR, "until.c")
OUT = os.path.join(ROOT_DIR, "bin", "test1-index-mode")

DEFAULT_STRIDE_LINES = 5
DEFAULT_TRAIN_STEP = None
DEFAULT_ROUNDS = 400
DEFAULT_PROBE_POSITIONS = 100
DEFAULT_REPEAT = 5
GREEN = "\033[32m"
RED = "\033[31m"
RESET = "\033[0m"
PMU_ARCHES = {"X925", "A725"}
L1D_HWPRF_COUNTER = "l1d_cache_hwprf"
L1D_HWPRF_YES_THRESHOLD = 1.0
PMU_RE = re.compile(
    r"^# PMU\s+(?P<name>\S+)\s+.*\bper_round="
    r"(?P<value>[0-9]+(?:\.[0-9]+)?)"
)

TESTS = (
    ("T0_no_trigger", 0),
    ("T0", 1),
    ("T1", 2),
    ("T2", 3),
    ("T3", 4),
    ("T4", 5),
    ("T5", 6),
    ("T6", 7),
)
TEST_DEFINE_BY_ID = dict(TESTS)

SUMMARY_LABELS = {
    "T0_no_trigger": "T0_no_trigger (sanity without trigger)",
    "T0": "T0 (same PC/VA/PA, noinline trigger)",
    "T1": "T1 (different PC, same VA/PA)",
    "T2": "T2 (same process, different VA, same PA)",
    "T3": "T3 (cross-process, same VA, different PA)",
    "T4": "T4 (same process, different PC/VA, same PA)",
    "T5": "T5 (same process remap, same PC/VA, different PA)",
    "T6": "T6 (same process remap, different PC, same VA, different PA)",
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
                        help="Override test0-style train_step. Default is selected from --arch and --access.")
    parser.add_argument("--access", choices=["store", "load"], default="store",
                        help="Stride instruction to test. Default: store")
    parser.add_argument("--rounds", type=int, default=DEFAULT_ROUNDS)
    parser.add_argument("--probe-positions", type=int,
                        default=DEFAULT_PROBE_POSITIONS)
    parser.add_argument("--repeat", type=int, default=DEFAULT_REPEAT,
                        help="Repeat train+trigger sequence inside each round. Default matches test0-exist.")
    parser.add_argument("--hit-threshold-ns", dest="threshold_ns", type=int,
                        default=None,
                        help=("Average latency <= threshold is classified as yes for non-PMU "
                              "architectures. X925/A725 use l1d_cache_hwprf instead. "
                              "Default comes from --arch."))
    parser.add_argument("--timer", choices=["gettime", "rdtsc", "cntvct", "pmccntr"],
                        default=None,
                        help="Timestamp source. Default comes from cross_test_config.py.")
    parser.add_argument("--test", choices=[test_id for test_id, _ in TESTS],
                        default=None,
                        help="Run one selected test. Default runs all tests by launching one C binary per test.")
    parser.add_argument("--cc", default=os.environ.get("CC", "gcc"))
    parser.add_argument("--pmu-device", default=None,
                        help="Override PMU_DEVICE for X925/A725 raw PMU events.")
    parser.add_argument("--no-compile", action="store_true")
    parser.add_argument("--verbose", action="store_true",
                        help="Print raw C output to stderr before summary.")
    args = parser.parse_args()

    if args.core is None:
        args.core = ARCH_CONFIG[args.arch]["core"]
    if args.train_step is None:
        args.train_step = ARCH_CONFIG[args.arch]["accesses"][args.access]
    if args.threshold_ns is None:
        args.threshold_ns = ARCH_CONFIG[args.arch]["threshold_ns"]
    if args.timer is None:
        args.timer = default_timer_for_arch(args.arch)

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


def compile_test(args, test_id):
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
        f"-DENABLE_CPP_RCTX={1 if args.arch in ('X925', 'A725') else 0}",
        f"-DPMU_CORE_X925={1 if args.arch == 'X925' else 0}",
        "-DPMU_CORE_A55=0",
        f"-DTRAIN_ACCESS_LOAD={1 if args.access == 'load' else 0}",
        f"-DSELECTED_TEST={TEST_DEFINE_BY_ID[test_id]}",
        "-o",
        OUT,
        PMU_SRC if args.arch in PMU_ARCHES else SRC,
        UTIL_SRC,
    ]
    timer_define = timer_define_for_arch(args.arch, args.timer)
    if timer_define is not None:
        cmd.insert(-4, timer_define)
    if args.arch in ("X925", "A725"):
        cmd.insert(5, "-march=armv8.5-a+predres")
    return subprocess.run(cmd)


def run_test(args):
    env = os.environ.copy()
    if args.pmu_device:
        env["PMU_DEVICE"] = args.pmu_device
    return subprocess.run([OUT], capture_output=True, text=True, env=env)


def parse_result(output, require_l1d_hwprf=False):
    row = None
    l1d_cache_hwprf = None
    for line in output.splitlines():
        stripped = line.strip()
        match = PMU_RE.match(stripped)
        if match:
            if match.group("name") == L1D_HWPRF_COUNTER:
                value = float(match.group("value"))
                l1d_cache_hwprf = (l1d_cache_hwprf or 0.0) + value
            continue
        if not stripped or stripped.startswith("#"):
            continue
        fields = stripped.split()
        if fields[0] == "id":
            continue
        if len(fields) != 6:
            raise ValueError(f"unexpected output row: {line}")
        if row is not None:
            raise ValueError(f"multiple result rows: previous {row[0]}, new {fields[0]}")
        row = (fields[0], int(fields[5]))

    if row is None:
        raise ValueError("missing result row")
    if require_l1d_hwprf and l1d_cache_hwprf is None:
        raise ValueError(
            "missing l1d_cache_hwprf PMU value; check perf_event permissions "
            "or --pmu-device"
        )
    return row[0], row[1], l1d_cache_hwprf


def is_prefetched(latency, threshold_ns, arch, l1d_cache_hwprf=None):
    if arch in PMU_ARCHES:
        return l1d_cache_hwprf >= L1D_HWPRF_YES_THRESHOLD
    return latency <= threshold_ns


def print_summary(test_id, latency, threshold_ns, timer_unit, arch,
                  l1d_cache_hwprf=None):
    prefetched = is_prefetched(latency, threshold_ns, arch, l1d_cache_hwprf)
    verdict = "yes" if prefetched else "no"
    color = GREEN if prefetched else RED
    pmu_text = (
        f" {L1D_HWPRF_COUNTER}={l1d_cache_hwprf:.2f}"
        if arch in PMU_ARCHES else ""
    )
    print(
        f"{SUMMARY_LABELS[test_id]} {color}{verdict}{RESET} "
        f"avg_{timer_unit}={latency}{pmu_text}"
    )


def is_prefetched_by_id(rows_by_id, test_id, threshold_ns, arch):
    result = rows_by_id[test_id]
    return is_prefetched(
        result["latency"], threshold_ns, arch, result["l1d_cache_hwprf"]
    )


def print_inference(rows_by_id, threshold_ns, arch):
    required = ("T0_no_trigger", "T0", "T1", "T2", "T3", "T4", "T5", "T6")
    missing = [test_id for test_id in required if test_id not in rows_by_id]

    print("\nInference:")
    if missing:
        print("  skip PC/VA/PA index inference: missing " + ", ".join(missing))
        return

    t0_no_trigger = is_prefetched_by_id(rows_by_id, "T0_no_trigger", threshold_ns, arch)
    t0 = is_prefetched_by_id(rows_by_id, "T0", threshold_ns, arch)
    t1 = is_prefetched_by_id(rows_by_id, "T1", threshold_ns, arch)
    t2 = is_prefetched_by_id(rows_by_id, "T2", threshold_ns, arch)
    t3 = is_prefetched_by_id(rows_by_id, "T3", threshold_ns, arch)
    t4 = is_prefetched_by_id(rows_by_id, "T4", threshold_ns, arch)
    t5 = is_prefetched_by_id(rows_by_id, "T5", threshold_ns, arch)
    t6 = is_prefetched_by_id(rows_by_id, "T6", threshold_ns, arch)

    if t0_no_trigger or not t0:
        print("  T0 sanity failed: expected T0_no_trigger=no and T0=yes.")
        print("  Do not infer PC/VA/PA index fields from this run.")
        return

    if t1:
        print("  PC: T1=yes when only trigger PC changes, so PC equality is not required.")
    else:
        print("  PC: T1=no when only trigger PC changes, so PC may participate in the index/match.")

    if t2:
        print("  VA: T2=yes with different VA but same PA, so VA equality is not required by this test.")
    else:
        print("  VA: T2=no with different VA but same PA, so VA may participate in the index/match.")

    if t5:
        print("  PA: T5=yes with same PC/VA but different PA, so PA equality is not required by this test.")
    else:
        print("  PA: T5=no with same PC/VA but different PA, so PA may participate in the index/match.")

    stronger = []
    if t4:
        stronger.append("PA-sufficient candidate: T4=yes when PC and VA both change but PA is same")
    if t6:
        stronger.append("VA-sufficient candidate: T6=yes when PC and PA both change but VA is same")
    if t3:
        stronger.append("VA/context-insensitive candidate: T3=yes across processes with same VA and different PA")

    if stronger:
        print("  stronger signals:")
        for line in stronger:
            print("  " + line)

    candidates = []
    if not t1:
        candidates.append("PC")
    if not t2:
        candidates.append("VA")
    if not t5:
        candidates.append("PA")
    if candidates:
        print("  likely indexed/matched fields: " + ", ".join(candidates))
    else:
        print("  likely indexed/matched fields: no single PC/VA/PA equality condition was required in this run.")


def main():
    args = parse_args()
    timer_unit = timer_unit_for_arch(args.arch, args.timer)
    tests_to_run = [args.test] if args.test is not None else [test_id for test_id, _ in TESTS]
    rows_by_id = {}

    if args.no_compile and len(tests_to_run) != 1:
        print("--no-compile requires --test because the binary contains one SELECTED_TEST", file=sys.stderr)
        return 1

    for selected_test in tests_to_run:
        if not args.no_compile:
            compile_result = compile_test(args, selected_test)
            if compile_result.returncode != 0:
                print(f"Compile failed for {selected_test}", file=sys.stderr)
                return compile_result.returncode

        run_result = run_test(args)
        if run_result.stderr:
            print(run_result.stderr, end="", file=sys.stderr)
        if run_result.returncode != 0:
            if run_result.stdout:
                print(run_result.stdout, end="", file=sys.stderr)
            print(f"Execution failed for {selected_test}", file=sys.stderr)
            return run_result.returncode

        if args.verbose:
            print(run_result.stdout, end="", file=sys.stderr)

        try:
            test_id, latency, l1d_cache_hwprf = parse_result(
                run_result.stdout,
                require_l1d_hwprf=args.arch in PMU_ARCHES,
            )
        except ValueError as exc:
            print(f"Parse failed for {selected_test}: {exc}", file=sys.stderr)
            return 1
        if test_id != selected_test:
            print(f"Expected {selected_test}, got {test_id}", file=sys.stderr)
            return 1

        print_summary(
            test_id, latency, args.threshold_ns, timer_unit,
            args.arch, l1d_cache_hwprf,
        )
        rows_by_id[test_id] = {
            "latency": latency,
            "l1d_cache_hwprf": l1d_cache_hwprf,
        }

    print_inference(rows_by_id, args.threshold_ns, args.arch)
    return 0


if __name__ == "__main__":
    sys.exit(main())
