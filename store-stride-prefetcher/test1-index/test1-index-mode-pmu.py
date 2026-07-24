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
SRC = os.path.join(BASE_DIR, "test1-index-mode-pmu.c")
UTIL_SRC = os.path.join(ROOT_DIR, "until.c")
OUT = os.path.join(ROOT_DIR, "bin", "test1-index-mode-pmu")

DEFAULT_STRIDE_LINES = 5
DEFAULT_TRAIN_STEP = None
DEFAULT_ROUNDS = 400
DEFAULT_PROBE_POSITIONS = 100
DEFAULT_REPEAT = 5
GREEN = "\033[32m"
RED = "\033[31m"
RESET = "\033[0m"

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
PMU_RE = re.compile(r"^# PMU\s+(\S+)\s+.*\bper_round=([0-9]+(?:\.[0-9]+)?)")
PMU_DISPLAY_NAMES = {
    "l1d_cache_refill_prefetch": "l1d_p",
    "l2d_cache_refill_prefetch": "l2d_p",
    "l3d_cache_refill_prefetch": "l3d_p",
}
L1D_PREFETCH_COUNTER = "l1d_cache_refill_prefetch"


def parse_args():
    parser = argparse.ArgumentParser(
        description="Run test1 index-mode PMU measurements one test at a time."
    )
    parser.add_argument("--arch", required=True, choices=arch_choices())
    parser.add_argument("--core", type=int, default=None,
                        help="Override CPU core. Default comes from --arch.")
    parser.add_argument("--stride", type=int, default=DEFAULT_STRIDE_LINES,
                        help="Stride in cache lines. Default: 5")
    parser.add_argument("--train-step", type=int, default=DEFAULT_TRAIN_STEP,
                        help="Override train_step. Default is selected from --arch and --access.")
    parser.add_argument("--access", choices=["store", "load"], default="store",
                        help="Stride instruction to test. Default: store")
    parser.add_argument("--rounds", type=int, default=DEFAULT_ROUNDS)
    parser.add_argument("--probe-positions", type=int,
                        default=DEFAULT_PROBE_POSITIONS)
    parser.add_argument("--repeat", type=int, default=DEFAULT_REPEAT,
                        help="Repeat train+trigger sequence inside each round.")
    parser.add_argument("--hit-threshold-ns", dest="threshold_ns", type=int,
                        default=None,
                        help="Average latency <= threshold is classified as prefetched.")
    parser.add_argument("--timer", choices=["gettime", "rdtsc", "cntvct", "pmccntr"],
                        default=None,
                        help="Timestamp source. Default comes from cross_test_config.py.")
    parser.add_argument("--tests", nargs="+", choices=[test_id for test_id, _ in TESTS],
                        default=[test_id for test_id, _ in TESTS],
                        help="Subset to run. Default: all tests.")
    parser.add_argument("--pmu-device", default=None,
                        help="Override PMU_DEVICE for pmu.h raw-event fallback.")
    parser.add_argument("--cc", default=os.environ.get("CC", "gcc"))
    parser.add_argument("--verbose", action="store_true",
                        help="Print raw C output for each test.")
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
        f"-DENABLE_CPP_RCTX={1 if args.arch == 'X925' else 0}",
        f"-DPMU_CORE_X925={1 if args.arch == 'X925' else 0}",
        f"-DPMU_CORE_A55={1 if args.arch == 'A55' else 0}",
        f"-DTRAIN_ACCESS_LOAD={1 if args.access == 'load' else 0}",
        f"-DSELECTED_TEST={TEST_DEFINE_BY_ID[test_id]}",
        "-o",
        OUT,
        SRC,
        UTIL_SRC,
    ]
    timer_define = timer_define_for_arch(args.arch, args.timer)
    if timer_define is not None:
        cmd.insert(-4, timer_define)
    if args.arch == "X925":
        cmd.insert(5, "-march=armv8.5-a+predres")
    result = subprocess.run(cmd, capture_output=True, text=True)
    if result.returncode != 0:
        if result.stdout:
            print(result.stdout, end="", file=sys.stderr)
        if result.stderr:
            print(result.stderr, end="", file=sys.stderr)
    return result.returncode


def run_test(args):
    env = os.environ.copy()
    if args.pmu_device:
        env["PMU_DEVICE"] = args.pmu_device
    return subprocess.run([OUT], capture_output=True, text=True, env=env)


def parse_output(output):
    row = None
    counters = {}
    notes = []
    for line in output.splitlines():
        stripped = line.strip()
        if not stripped:
            continue
        match = PMU_RE.match(stripped)
        if match:
            counters[match.group(1)] = counters.get(match.group(1), 0.0) + float(match.group(2))
            continue
        if stripped.startswith("# PMU unavailable") or stripped.startswith("# PMU note"):
            notes.append(stripped[2:])
            continue
        if stripped.startswith("#") or stripped.startswith("id\t"):
            continue
        fields = stripped.split()
        if len(fields) != 6:
            raise ValueError(f"unexpected output row: {line}")
        row = {
            "id": fields[0],
            "description": fields[1],
            "pc": fields[2],
            "va": fields[3],
            "pa": fields[4],
            "latency": int(fields[5]),
        }
    if row is None:
        raise ValueError("missing result row")
    return row, counters, notes


def print_summary(results, threshold, timer_unit):
    counter_names = sorted({name for result in results for name in result["pmu"]})
    print_test_descriptions()
    print()
    columns = ["test", f"avg_{timer_unit}", "prefetched"]
    columns.extend(PMU_DISPLAY_NAMES.get(name, name) for name in counter_names)
    print("\t".join(columns))
    for result in results:
        prefetched = is_prefetched(result, threshold)
        verdict = "yes" if prefetched else "no"
        color = GREEN if prefetched else RED
        values = [
            result["id"],
            str(result["latency"]),
            f"{color}{verdict}{RESET}",
        ]
        values.extend(f"{result['pmu'].get(name, 0.0):.2f}" for name in counter_names)
        print("\t".join(values))

    notes = []
    for result in results:
        for note in result["notes"]:
            notes.append(f"{result['id']}: {note}")
    if notes:
        print("\nnotes:")
        for note in notes:
            print(note)

    print_inference(results, threshold)


def is_prefetched(result, threshold):
    return result["latency"] < threshold or result["pmu"].get(L1D_PREFETCH_COUNTER, 0.0) >= 1.0


def is_prefetched_by_id(rows_by_id, test_id, threshold):
    return is_prefetched(rows_by_id[test_id], threshold)


def print_inference(results, threshold):
    rows_by_id = {result["id"]: result for result in results}
    required = ("T0_no_trigger", "T0", "T1", "T2", "T3", "T4", "T5", "T6")
    missing = [test_id for test_id in required if test_id not in rows_by_id]

    print("\ninference:")
    if missing:
        print("  skip PC/VA/PA index inference: missing " + ", ".join(missing))
        return

    t0_no_trigger = is_prefetched_by_id(rows_by_id, "T0_no_trigger", threshold)
    t0 = is_prefetched_by_id(rows_by_id, "T0", threshold)
    t1 = is_prefetched_by_id(rows_by_id, "T1", threshold)
    t2 = is_prefetched_by_id(rows_by_id, "T2", threshold)
    t3 = is_prefetched_by_id(rows_by_id, "T3", threshold)
    t4 = is_prefetched_by_id(rows_by_id, "T4", threshold)
    t5 = is_prefetched_by_id(rows_by_id, "T5", threshold)
    t6 = is_prefetched_by_id(rows_by_id, "T6", threshold)

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

    sufficient = []
    if t4:
        sufficient.append("PA-sufficient candidate: T4=yes when PC and VA both change but PA is same")
    if t6:
        sufficient.append("VA-sufficient candidate: T6=yes when PC and PA both change but VA is same")
    if t3:
        sufficient.append("VA/context-insensitive candidate: T3=yes across processes with same VA and different PA")

    if sufficient:
        print("  stronger signals:")
        for line in sufficient:
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


def print_test_descriptions():
    print("\ntests:")
    print("  T0_no_trigger: same PC/VA/PA, but no trigger access; sanity negative control.")
    print("  T0: same PC/VA/PA with noinline trigger; sanity positive control.")
    print("  T1: different trigger PC, same VA/PA; tests whether PC equality is required.")
    print("  T2: same PC, different VA, same PA; tests whether VA equality is required.")
    print("  T3: cross-process, same VA, different PA; tests VA matching across address spaces.")
    print("  T4: different PC and VA, same PA; tests whether PA alone can be sufficient.")
    print("  T5: same PC/VA, different PA via remap; tests whether PA equality is required.")
    print("  T6: different PC and PA, same VA via remap; tests whether VA alone can be sufficient.")


def main():
    args = parse_args()
    timer_unit = timer_unit_for_arch(args.arch, args.timer)
    results = []

    for test_id in args.tests:
        if args.verbose:
            print(f"## running {test_id}", file=sys.stderr)
        compile_rc = compile_test(args, test_id)
        if compile_rc != 0:
            print(f"Compile failed for {test_id}", file=sys.stderr)
            return compile_rc

        run_result = run_test(args)
        if args.verbose and run_result.stdout:
            print(run_result.stdout, end="", file=sys.stderr)
        if run_result.stderr:
            print(run_result.stderr, end="", file=sys.stderr)
        if run_result.returncode != 0:
            if not args.verbose and run_result.stdout:
                print(run_result.stdout, end="", file=sys.stderr)
            print(f"Execution failed for {test_id}", file=sys.stderr)
            return run_result.returncode

        try:
            row, counters, notes = parse_output(run_result.stdout)
        except ValueError as exc:
            print(f"Parse failed for {test_id}: {exc}", file=sys.stderr)
            return 1
        if row["id"] != test_id:
            print(f"Expected {test_id}, got {row['id']}", file=sys.stderr)
            return 1
        row["pmu"] = counters
        row["notes"] = notes
        results.append(row)

    print_summary(results, args.threshold_ns, timer_unit)
    return 0


if __name__ == "__main__":
    sys.exit(main())
