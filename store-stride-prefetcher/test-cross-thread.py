import argparse
import csv
import os
import subprocess
import sys

from cross_test_config import (
    apply_single_core_defaults,
    apply_train_access_defaults,
    arch_choices,
)
from cross_plot import plot_cross_bar_chart

BASE_DIR = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.join(BASE_DIR, "test-cross-thread.c")
UTIL_SRC = os.path.join(BASE_DIR, "until.c")
OUT = os.path.join(BASE_DIR, "bin", "test-cross-thread")

DEFAULT_STRIDE_LINES = 5
DEFAULT_ROUNDS = 4000
DEFAULT_PROBE_POSITIONS = 64
DEFAULT_HIT_THRESHOLD_NS = 150

def parse_args():
    parser = argparse.ArgumentParser(
        description="Compile, run, and plot cross-thread stride test."
    )
    parser.add_argument("--arch", required=True, choices=arch_choices())
    parser.add_argument("--core", type=int, default=None,
                        help="Override CPU core. Default is selected from --arch.")
    parser.add_argument("--stride", type=int, default=DEFAULT_STRIDE_LINES)
    parser.add_argument("--train-accesses", type=int,
                        default=None,
                        help="Total accesses including the trigger. "
                             "Default is selected from --arch and --access.")
    parser.add_argument("--rounds", type=int, default=DEFAULT_ROUNDS)
    parser.add_argument("--probe-positions", type=int,
                        default=DEFAULT_PROBE_POSITIONS)
    parser.add_argument("--hit-threshold-ns", type=int,
                        default=DEFAULT_HIT_THRESHOLD_NS)
    parser.add_argument("--access", choices=["store", "load"], default="store",
                        help="Stride instruction to test. Default: store")
    parser.add_argument("--inline-store", action="store_true",
                        help="Use inline store call sites instead of same-PC "
                             "noinline stores.")
    parser.add_argument("--cc", default=os.environ.get("CC", "gcc"))
    parser.add_argument("--plot-only", action="store_true")
    parser.add_argument("--no-plot", action="store_true")
    args = parser.parse_args()

    apply_single_core_defaults(args)
    apply_train_access_defaults(args)

    if args.core < 0:
        parser.error("--core must be >= 0")
    if args.stride < 1:
        parser.error("--stride must be >= 1")
    if args.train_accesses < 1:
        parser.error("--train-accesses must be >= 1")
    if args.access == "load" and args.train_accesses < 2:
        parser.error("--train-accesses must be >= 2 for load")
    if args.rounds < 1:
        parser.error("--rounds must be >= 1")
    if args.probe_positions < 1 or args.probe_positions > 64:
        parser.error("--probe-positions must be in [1, 64]")
    if args.hit_threshold_ns < 1:
        parser.error("--hit-threshold-ns must be >= 1")

    if args.access == "load":
        predicted_line = args.train_accesses * args.stride
    else:
        predicted_line = (args.train_accesses + 1) * args.stride
    if predicted_line >= 64:
        parser.error("train/trigger/predicted lines must fit in one 4KB page")

    return args


args = parse_args()
result_dir = os.path.join(BASE_DIR, "res", "cross-thread")
plot_dir = os.path.join(BASE_DIR, "res", "barplots")
raw_dir = os.path.join(result_dir, "raw")


def train_only_accesses():
    if args.access == "load":
        return args.train_accesses - 1
    return args.train_accesses


def trigger_line():
    if args.access == "load":
        return (args.train_accesses - 1) * args.stride
    return args.train_accesses * args.stride


def predicted_line():
    return trigger_line() + args.stride


def micro_arch_name():
    if args.access == "load":
        pc_mode = "noinline"
    else:
        pc_mode = "inline" if args.inline_store else "noinline"
    return (
        f"{args.arch}-core{args.core}-cross-thread"
        f"-stride{args.stride}-train{args.train_accesses}"
        f"-probe{args.probe_positions}-{args.access}-{pc_mode}"
    )


def tsv_path():
    return os.path.join(result_dir, f"{micro_arch_name()}.tsv")


def raw_path():
    return os.path.join(raw_dir, f"{micro_arch_name()}.txt")


def plot_path():
    return os.path.join(plot_dir, f"{micro_arch_name()}-avg_ns.png")


def control_plot_path():
    return os.path.join(
        plot_dir, f"{micro_arch_name()}-no-trigger-control-avg_ns.png"
    )


def control_tsv_path():
    return os.path.join(result_dir, f"{micro_arch_name()}-no-trigger-control.tsv")


def control_raw_path():
    return os.path.join(raw_dir, f"{micro_arch_name()}-no-trigger-control.txt")


def ensure_dirs():
    os.makedirs(os.path.dirname(OUT), exist_ok=True)
    os.makedirs(result_dir, exist_ok=True)
    os.makedirs(plot_dir, exist_ok=True)
    os.makedirs(raw_dir, exist_ok=True)


def trained_offsets():
    return {
        step * args.stride * 64 for step in range(train_only_accesses())
    }


def trigger_offset():
    return trigger_line() * 64


def predicted_offset():
    return predicted_line() * 64


def classify_position(offset_bytes, avg_ns, probes):
    if offset_bytes in trained_offsets():
        return "trained"
    if offset_bytes == trigger_offset():
        return "trigger"
    if offset_bytes == predicted_offset():
        return "predicted"
    if probes > 0 and avg_ns <= args.hit_threshold_ns:
        return "prefetched"
    return "cache_miss"


def parse_output(output):
    rows = []

    for line in output.splitlines():
        stripped = line.strip()
        if not stripped or stripped.startswith("#"):
            continue

        fields = stripped.split()
        if len(fields) != 4:
            print(f"Skipping unexpected output row: {line}", file=sys.stderr)
            continue

        try:
            position = int(fields[0])
            offset_bytes = int(fields[1])
            avg_ns = int(fields[2])
            probes = int(fields[3])
        except ValueError:
            print(f"Skipping non-numeric output row: {line}", file=sys.stderr)
            continue

        rows.append({
            "position": position,
            "offset_bytes": offset_bytes,
            "role": classify_position(offset_bytes, avg_ns, probes),
            "avg_ns": avg_ns,
            "probes": probes,
        })

    return rows


def write_tsv(rows, path):
    with open(path, "w", newline="") as f:
        writer = csv.DictWriter(
            f,
            fieldnames=["position", "offset_bytes", "role", "avg_ns", "probes"],
            delimiter="\t",
        )
        writer.writeheader()
        writer.writerows(rows)


def read_tsv(path):
    rows = []

    with open(path, newline="") as f:
        reader = csv.DictReader(f, delimiter="\t")
        for row in reader:
            rows.append({
                "position": int(row["position"]),
                "offset_bytes": int(row["offset_bytes"]),
                "role": row["role"],
                "avg_ns": int(row["avg_ns"]),
                "probes": int(row["probes"]),
            })
    return rows


def compile_test(no_trigger=False):
    use_noinline = 0 if args.inline_store else 1
    train_access_load = 1 if args.access == "load" else 0
    compile_cmd = [
        args.cc,
        "-std=gnu11",
        "-O0",
        "-static",
        "-pthread",
        f"-DSTRIDE_LINES={args.stride}",
        f"-DTRAIN_ACCESSES={args.train_accesses}",
        f"-DROUNDS={args.rounds}",
        f"-DPROBE_POSITIONS={args.probe_positions}",
        f"-DCPU_ID={args.core}",
        f"-DUSE_NOINLINE_STORE={use_noinline}",
        f"-DTRAIN_ACCESS_LOAD={train_access_load}",
        f"-DNO_TRIGGER={1 if no_trigger else 0}",
        "-o",
        OUT,
        SRC,
        UTIL_SRC,
    ]
    return subprocess.run(compile_cmd)


def run_binary():
    return subprocess.run(
        ["taskset", "-c", str(args.core), OUT],
        capture_output=True,
        text=True,
    )


def run_one(no_trigger=False):
    compiled = compile_test(no_trigger=no_trigger)
    if compiled.returncode != 0:
        print("Compile failed")
        return []

    run = run_binary()
    if run.returncode != 0:
        print("Execution failed")
        if run.stdout:
            print(run.stdout)
        if run.stderr:
            print(run.stderr)
        return []

    path = control_raw_path() if no_trigger else raw_path()
    with open(path, "w") as f:
        f.write(run.stdout)

    rows = parse_output(run.stdout)
    if not rows:
        print("No parsed rows")
        return []

    write_tsv(rows, control_tsv_path() if no_trigger else tsv_path())
    return rows


def no_trigger_baseline(rows=None):
    if rows is None:
        rows = run_one(no_trigger=True)
    if not rows:
        return None

    predicted = predicted_line()
    print_summary(rows, "No-trigger result:")
    for row in rows:
        if row["position"] == predicted:
            print(
                f"No-trigger control: line {predicted} "
                f"avg_ns={row['avg_ns']}"
            )
            return row["avg_ns"]

    print(f"No-trigger control missing line {predicted}")
    return None


def print_summary(rows, label):
    targets = [0, 5, 10, 15, 20, 25, 30, 35, 40, 45]
    print(label)
    for pos in targets:
        if pos < len(rows):
            row = rows[pos]
            print(f"  line {pos:2d}: {row['role']:10s} {row['avg_ns']:4d} ns")


def run_test():
    print("=" * 60)
    print(
        f"cross-thread {args.access}-stride, arch={args.arch}, core={args.core}, "
        f"stride={args.stride} lines, train_accesses={args.train_accesses}, "
        f"train_only_accesses={train_only_accesses()}, "
        f"trigger_line={trigger_line()}, predicted_line={predicted_line()}, "
        f"probe_positions={args.probe_positions}, rounds={args.rounds}, "
        f"{args.access}={'noinline' if args.access == 'load' else ('inline' if args.inline_store else 'noinline')}"
    )

    rows = run_one(no_trigger=False)
    if not rows:
        return []

    print(f"Saved raw output to {raw_path()}")
    print(f"Saved parsed results to {tsv_path()}")
    print_summary(rows, "Trigger result:")
    return rows


def plot_bar_chart(rows, no_trigger_avg_ns=None, title=None, path=None):
    access_mode = (
        "noinline" if args.access == "load"
        else ("inline" if args.inline_store else "noinline")
    )
    if path is None:
        path = plot_path()
    plot_cross_bar_chart(
        rows,
        args=args,
        trigger_line=trigger_line(),
        predicted_line=predicted_line(),
        train_only_accesses=train_only_accesses(),
        default_title=f"Thread0 train, thread1 {args.access} trigger",
        trained_label="thread0 trained",
        trigger_label=f"thread1 {args.access} trigger",
        summary_text=(
            f"{args.arch} core {args.core}, stride={args.stride}, "
            f"train_only={train_only_accesses()}, "
            f"trigger_accesses={args.train_accesses}, "
            f"{args.access}={access_mode}"
        ),
        output_path=path,
        no_trigger_avg_ns=no_trigger_avg_ns,
        title=title,
    )


if __name__ == "__main__":
    ensure_dirs()
    baseline_avg_ns = None
    control_rows = None

    if args.plot_only:
        if not os.path.exists(tsv_path()):
            print(f"Error: TSV result '{tsv_path()}' not found.")
            sys.exit(1)
        result_rows = read_tsv(tsv_path())
        if os.path.exists(control_tsv_path()):
            control_rows = read_tsv(control_tsv_path())
            baseline_avg_ns = control_rows[predicted_line()]["avg_ns"]
            print_summary(control_rows, "Existing no-trigger result:")
        print_summary(result_rows, "Existing trigger result:")
    else:
        control_rows = run_one(no_trigger=True)
        baseline_avg_ns = no_trigger_baseline(control_rows)
        result_rows = run_test()

    if control_rows and not args.no_plot:
        plot_bar_chart(
            control_rows,
            None,
            title="Thread0 train, no thread1 trigger",
            path=control_plot_path(),
        )

    if result_rows and not args.no_plot:
        plot_bar_chart(result_rows, baseline_avg_ns)
