import argparse
import csv
import os
import subprocess
import sys

ROOT_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
if ROOT_DIR not in sys.path:
    sys.path.insert(0, ROOT_DIR)

from cross_test_config import (
    apply_access_defaults,
    apply_single_core_defaults,
    apply_threshold_defaults,
    arch_choices,
    is_x86_arch,
    timer_define_for_arch,
    timer_unit_for_arch,
)
from cross_plot import plot_cross_bar_chart
from cross_result_eval import (
    compute_cross_threshold,
    print_cross_evaluation,
    reclassify_cross_rows,
)

BASE_DIR = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.join(BASE_DIR, "test3-cross-process.c")
UTIL_SRC = os.path.join(ROOT_DIR, "until.c")
OUT = os.path.join(ROOT_DIR, "bin", "test3-cross-process")

DEFAULT_STRIDE_LINES = 5
DEFAULT_ROUNDS = 4000
DEFAULT_PROBE_POSITIONS = 64
DEFAULT_CHILD_MAP_ADDR = "0x700000000"
ANSI_GREEN = "\033[32m"
ANSI_RESET = "\033[0m"

def parse_int_auto(value):
    try:
        return int(value, 0)
    except ValueError as exc:
        raise argparse.ArgumentTypeError(str(exc)) from exc


def parse_args():
    parser = argparse.ArgumentParser(
        description="Compile, run, and plot strong cross-process stride test."
    )
    parser.add_argument("--arch", required=True, choices=arch_choices())
    parser.add_argument("--core", type=int, default=None,
                        help="Override CPU core. Default is selected from --arch.")
    parser.add_argument("--stride", type=int, default=DEFAULT_STRIDE_LINES,
                        help="Stride in cache lines. Default: 5")
    parser.add_argument("--accesses", type=int,
                        default=None,
                        help="Total train+trigger accesses. Default is selected "
                             "from --arch and --access.")
    parser.add_argument("--train-accesses", type=int,
                        default=None,
                        help="Deprecated alias for --accesses.")
    parser.add_argument("--trigger-accesses", type=int, default=1,
                        help="Number of trigger accesses at the end of the stride sequence.")
    parser.add_argument("--rounds", type=int, default=DEFAULT_ROUNDS)
    parser.add_argument("--probe-positions", type=int,
                        default=DEFAULT_PROBE_POSITIONS)
    parser.add_argument("--hit-threshold-ns", type=int,
                        default=None,
                        help="Override the arch-specific cache hit threshold.")
    parser.add_argument("--timer", choices=["gettime", "rdtsc"],
                        default="gettime",
                        help=("x86 timestamp source. gettime uses "
                              "clock_gettime; rdtsc uses rdtscp. "
                              "Default: gettime"))
    parser.add_argument("--access", choices=["store", "load"], default="store",
                        help="Stride instruction to test. Default: store")
    parser.add_argument("--child-map-addr", type=parse_int_auto,
                        default=parse_int_auto(DEFAULT_CHILD_MAP_ADDR),
                        help="Requested child mmap address. Default: 0x700000000")
    parser.add_argument("--inline-store", action="store_true",
                        help="Use inline store call sites instead of same-PC "
                             "noinline stores.")
    parser.add_argument("--cc", default=os.environ.get("CC", "gcc"))
    parser.add_argument("--plot-only", action="store_true")
    parser.add_argument("--no-plot", action="store_true")
    args = parser.parse_args()

    if args.accesses is not None and args.train_accesses is not None:
        parser.error("use only one of --accesses or deprecated --train-accesses")
    if args.accesses is None and args.train_accesses is not None:
        args.accesses = args.train_accesses

    apply_single_core_defaults(args)
    apply_access_defaults(args)
    apply_threshold_defaults(args)

    if args.core < 0:
        parser.error("--core must be >= 0")
    if args.stride < 1:
        parser.error("--stride must be >= 1")
    if args.accesses < 1:
        parser.error("--accesses must be >= 1")
    if args.trigger_accesses < 1 or args.trigger_accesses > 2:
        parser.error("--trigger-accesses must be 1 or 2")
    if args.trigger_accesses >= args.accesses:
        parser.error("--trigger-accesses must be smaller than --accesses")
    if args.rounds < 1:
        parser.error("--rounds must be >= 1")
    if args.probe_positions < 1 or args.probe_positions > 64:
        parser.error("--probe-positions must be in [1, 64]")
    if args.hit_threshold_ns < 1:
        parser.error("--hit-threshold-ns must be >= 1")
    if args.child_map_addr < 0:
        parser.error("--child-map-addr must be >= 0")
    args.timer_unit = timer_unit_for_arch(args.arch, args.timer)

    predicted_line = args.accesses * args.stride
    if predicted_line >= 64:
        parser.error("train/trigger/predicted lines must fit in one 4KB page")

    return args


args = parse_args()
result_dir = os.path.join(ROOT_DIR, "res", "cross-process-strong")
plot_dir = os.path.join(ROOT_DIR, "res", "barplots")
raw_dir = os.path.join(result_dir, "raw")


def micro_arch_name():
    if args.access == "load":
        pc_mode = "noinline"
    else:
        pc_mode = "inline" if args.inline_store else "noinline"
    child_addr = f"childva{args.child_map_addr:x}"
    timer_suffix = ""
    if is_x86_arch(args.arch) and args.timer != "gettime":
        timer_suffix = f"-timer{args.timer}"
    return (
        f"{args.arch}-core{args.core}-cross-process-strong"
        f"-stride{args.stride}-accesses{args.accesses}"
        f"-trigger{args.trigger_accesses}"
        f"-probe{args.probe_positions}-{args.access}-{pc_mode}-{child_addr}"
        f"{timer_suffix}"
    )


def tsv_path():
    return experiment4_tsv_path()


def raw_path():
    return experiment4_raw_path()


def plot_path():
    return experiment4_plot_path()


def baseline1_plot_path():
    return os.path.join(plot_dir, f"{micro_arch_name()}-baseline1-parent-trigger-avg_ns.png")


def baseline2_plot_path():
    return os.path.join(plot_dir, f"{micro_arch_name()}-baseline2-no-trigger-avg_ns.png")


def experiment3_plot_path():
    return os.path.join(plot_dir, f"{micro_arch_name()}-exp3-child-switch-parent-trigger-avg_ns.png")


def experiment4_plot_path():
    return os.path.join(plot_dir, f"{micro_arch_name()}-exp4-child-trigger-avg_ns.png")


def baseline1_tsv_path():
    return os.path.join(result_dir, f"{micro_arch_name()}-baseline1-parent-trigger.tsv")


def baseline1_raw_path():
    return os.path.join(raw_dir, f"{micro_arch_name()}-baseline1-parent-trigger.txt")


def baseline2_tsv_path():
    return os.path.join(result_dir, f"{micro_arch_name()}-baseline2-no-trigger.tsv")


def baseline2_raw_path():
    return os.path.join(raw_dir, f"{micro_arch_name()}-baseline2-no-trigger.txt")


def experiment3_tsv_path():
    return os.path.join(result_dir, f"{micro_arch_name()}-exp3-child-switch-parent-trigger.tsv")


def experiment3_raw_path():
    return os.path.join(raw_dir, f"{micro_arch_name()}-exp3-child-switch-parent-trigger.txt")


def experiment4_tsv_path():
    return os.path.join(result_dir, f"{micro_arch_name()}-exp4-child-trigger.tsv")


def experiment4_raw_path():
    return os.path.join(raw_dir, f"{micro_arch_name()}-exp4-child-trigger.txt")


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


def train_only_accesses():
    return args.accesses - args.trigger_accesses


def trigger_line():
    return train_only_accesses() * args.stride


def last_trigger_line():
    return (args.accesses - 1) * args.stride


def predicted_line():
    return args.accesses * args.stride


def trigger_offsets():
    return {
        (train_only_accesses() + trigger_index) * args.stride * 64
        for trigger_index in range(args.trigger_accesses)
    }


def classify_position(offset_bytes, avg_ns, probes):
    if offset_bytes in trained_offsets():
        return "trained"
    if offset_bytes in trigger_offsets():
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


def compile_test(no_trigger=False, same_process=False, process_switch=False):
    use_noinline = 0 if args.inline_store else 1
    train_access_load = 1 if args.access == "load" else 0
    compile_cmd = [
        args.cc,
        "-std=gnu11",
        "-O0",
        "-static",
         "-march=armv8.5-a+predres",
        f"-DSTRIDE_LINES={args.stride}",
        f"-DTRAIN_ACCESSES={train_only_accesses()}",
        f"-DTRIGGER_ACCESSES={args.trigger_accesses}",
        f"-DROUNDS={args.rounds}",
        f"-DPROBE_POSITIONS={args.probe_positions}",
        f"-DCPU_ID={args.core}",
        f"-DUSE_NOINLINE_STORE={use_noinline}",
        f"-DTRAIN_ACCESS_LOAD={train_access_load}",
        f"-DNO_TRIGGER={1 if no_trigger else 0}",
        f"-DSAME_PROCESS_TRIGGER={1 if same_process else 0}",
        f"-DPROCESS_SWITCH_ONLY={1 if process_switch else 0}",
        f"-DCHILD_MAP_ADDR=0x{args.child_map_addr:x}ULL",
        "-o",
        OUT,
        SRC,
        UTIL_SRC,
    ]
    timer_define = timer_define_for_arch(args.arch, args.timer)
    if timer_define is not None:
        compile_cmd.insert(-4, timer_define)
    return subprocess.run(compile_cmd)


def run_binary():
    return subprocess.run(
        ["taskset", "-c", str(args.core), OUT],
        capture_output=True,
        text=True,
    )


def result_paths(no_trigger=False, same_process=False, process_switch=False):
    if no_trigger:
        return baseline2_raw_path(), baseline2_tsv_path()
    if process_switch:
        return experiment3_raw_path(), experiment3_tsv_path()
    if same_process:
        return baseline1_raw_path(), baseline1_tsv_path()
    return experiment4_raw_path(), experiment4_tsv_path()


def run_one(no_trigger=False, same_process=False, process_switch=False):
    compiled = compile_test(no_trigger=no_trigger,
                            same_process=same_process,
                            process_switch=process_switch)
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

    raw_result_path, tsv_result_path = result_paths(
        no_trigger=no_trigger,
        same_process=same_process,
        process_switch=process_switch,
    )
    with open(raw_result_path, "w") as f:
        f.write(run.stdout)

    rows = parse_output(run.stdout)
    if not rows:
        print("No parsed rows")
        return []

    write_tsv(rows, tsv_result_path)
    return rows


def print_summary(rows, label):
    targets = [
        step * args.stride
        for step in range(args.accesses + 4)
        if step * args.stride < args.probe_positions
    ]
    print(label)
    for pos in targets:
        if pos < len(rows):
            row = rows[pos]
            print(
                f"  line {pos:2d}: {row['role']:10s} "
                f"{row['avg_ns']:4d} {args.timer_unit}"
            )


def print_green(text):
    print(f"{ANSI_GREEN}{text}{ANSI_RESET}")


def effective_threshold_and_reclassify(*row_sets):
    threshold_ns, threshold_source = compute_cross_threshold(
        configured_threshold_ns=args.hit_threshold_ns,
        auto_threshold=args.hit_threshold_ns_auto,
        baseline1_rows=row_sets[0] if len(row_sets) > 0 else None,
        baseline2_rows=row_sets[1] if len(row_sets) > 1 else None,
        unit=args.timer_unit,
    )
    for rows in row_sets:
        reclassify_cross_rows(rows, threshold_ns)
    return threshold_ns, threshold_source


def plot_bar_chart(rows, no_trigger_avg_ns=None, title=None, path=None,
                   trigger_label=None):
    access_mode = (
        "noinline" if args.access == "load"
        else ("inline" if args.inline_store else "noinline")
    )
    if path is None:
        path = plot_path()
    if trigger_label is None:
        trigger_label = f"exec child {args.access} trigger"
    plot_cross_bar_chart(
        rows,
        args=args,
        trigger_line=trigger_line(),
        predicted_line=predicted_line(),
        train_only_accesses=train_only_accesses(),
        default_title=f"Strong cross-process {args.access} trigger",
        trained_label="parent trained",
        trigger_label=trigger_label,
        summary_text=(
            f"{args.arch} core {args.core}, stride={args.stride}, "
            f"accesses={args.accesses}, "
            f"train_only={train_only_accesses()}, "
            f"trigger_accesses={args.trigger_accesses}, "
            f"{args.access}={access_mode}, "
            f"timer={args.timer if is_x86_arch(args.arch) else 'arch-default'}, "
            f"child_va=0x{args.child_map_addr:x}"
        ),
        output_path=path,
        no_trigger_avg_ns=no_trigger_avg_ns,
        title=title,
    )


if __name__ == "__main__":
    ensure_dirs()
    baseline_avg_ns = None
    if args.plot_only:
        baseline1_rows = None
        baseline2_rows = None
        experiment3_rows = None
        experiment4_rows = None
        if os.path.exists(baseline2_tsv_path()):
            baseline2_rows = read_tsv(baseline2_tsv_path())
            baseline_avg_ns = baseline2_rows[predicted_line()]["avg_ns"]
            print_summary(baseline2_rows, "Existing baseline2 no-trigger result:")
        if os.path.exists(baseline1_tsv_path()):
            baseline1_rows = read_tsv(baseline1_tsv_path())
            print_summary(baseline1_rows, "Existing baseline1 parent-trigger result:")
            if not args.no_plot:
                plot_bar_chart(
                    baseline1_rows,
                    baseline_avg_ns,
                    title="Baseline1: parent train and trigger",
                    path=baseline1_plot_path(),
                    trigger_label=f"parent {args.access} trigger",
                )
        if os.path.exists(experiment3_tsv_path()):
            experiment3_rows = read_tsv(experiment3_tsv_path())
            print_summary(experiment3_rows, "Existing experiment3 process-switch result:")
            if not args.no_plot:
                plot_bar_chart(
                    experiment3_rows,
                    baseline_avg_ns,
                    title="Experiment3: child process switch, parent trigger",
                    path=experiment3_plot_path(),
                    trigger_label=f"parent {args.access} trigger after process switch",
                )
        if os.path.exists(experiment4_tsv_path()):
            experiment4_rows = read_tsv(experiment4_tsv_path())
            print_summary(experiment4_rows, "Existing experiment4 child-trigger result:")
            if not args.no_plot:
                plot_bar_chart(
                    experiment4_rows,
                    baseline_avg_ns,
                    title="Experiment4: parent train, child trigger",
                    path=experiment4_plot_path(),
                )
        threshold_ns, threshold_source = effective_threshold_and_reclassify(
            baseline1_rows,
            baseline2_rows,
            experiment3_rows,
            experiment4_rows,
        )
        print_cross_evaluation(
            predicted_line=predicted_line(),
            threshold_ns=threshold_ns,
            threshold_source=threshold_source,
            baseline1_rows=baseline1_rows,
            baseline2_rows=baseline2_rows,
            experiment3_rows=experiment3_rows,
            experiment4_rows=experiment4_rows,
            unit=args.timer_unit,
        )
        sys.exit(0)

    print("=" * 60)
    print(
        f"strong cross-process {args.access}-stride, arch={args.arch}, core={args.core}, "
        f"stride={args.stride} lines, accesses={args.accesses}, "
        f"train_only_accesses={train_only_accesses()}, "
        f"trigger_accesses={args.trigger_accesses}, "
        f"trigger_line={trigger_line()}, last_trigger_line={last_trigger_line()}, "
        f"predicted_line={predicted_line()}, "
        f"probe_positions={args.probe_positions}, rounds={args.rounds}, "
        f"timer={args.timer if is_x86_arch(args.arch) else 'arch-default'}, "
        f"unit={args.timer_unit}, "
        f"{args.access}={'noinline' if args.access == 'load' else ('inline' if args.inline_store else 'noinline')}, "
        f"child_map_addr=0x{args.child_map_addr:x}"
    )

    baseline1_rows = run_one(same_process=True)
    if baseline1_rows:
        predicted = predicted_line()
        print_green(
            f"Baseline1 parent-trigger: line {predicted} "
            f"avg_{args.timer_unit}={baseline1_rows[predicted]['avg_ns']}"
        )
        print_summary(baseline1_rows, "Baseline1 parent-trigger result:")

    baseline2_rows = run_one(no_trigger=True, same_process=True)
    if baseline2_rows:
        predicted = predicted_line()
        baseline_avg_ns = baseline2_rows[predicted]["avg_ns"]
        print_green(
            f"Baseline2 no-trigger: line {predicted} "
            f"avg_{args.timer_unit}={baseline_avg_ns}"
        )
        print_summary(baseline2_rows, "Baseline2 no-trigger result:")

    experiment3_rows = run_one(process_switch=True)
    if experiment3_rows:
        predicted = predicted_line()
        print_green(
            f"Experiment3 process-switch parent-trigger: line {predicted} "
            f"avg_{args.timer_unit}={experiment3_rows[predicted]['avg_ns']}"
        )
        print_summary(experiment3_rows, "Experiment3 process-switch result:")

    experiment4_rows = run_one()
    if experiment4_rows:
        predicted = predicted_line()
        print_green(
            f"Experiment4 child-trigger: line {predicted} "
            f"avg_{args.timer_unit}={experiment4_rows[predicted]['avg_ns']}"
        )
        print_summary(experiment4_rows, "Experiment4 child-trigger result:")

    threshold_ns, threshold_source = effective_threshold_and_reclassify(
        baseline1_rows,
        baseline2_rows,
        experiment3_rows,
        experiment4_rows,
    )
    print_cross_evaluation(
        predicted_line=predicted_line(),
        threshold_ns=threshold_ns,
        threshold_source=threshold_source,
        baseline1_rows=baseline1_rows,
        baseline2_rows=baseline2_rows,
        experiment3_rows=experiment3_rows,
        experiment4_rows=experiment4_rows,
        unit=args.timer_unit,
    )

    if not args.no_plot:
        if baseline1_rows:
            plot_bar_chart(
                baseline1_rows,
                baseline_avg_ns,
                title="Baseline1: parent train and trigger",
                path=baseline1_plot_path(),
                trigger_label=f"parent {args.access} trigger",
            )
        if baseline2_rows:
            plot_bar_chart(
                baseline2_rows,
                None,
                title="Baseline2: parent train, no trigger",
                path=baseline2_plot_path(),
                trigger_label="no trigger",
            )
        if experiment3_rows:
            plot_bar_chart(
                experiment3_rows,
                baseline_avg_ns,
                title="Experiment3: child process switch, parent trigger",
                path=experiment3_plot_path(),
                trigger_label=f"parent {args.access} trigger after process switch",
            )
        if experiment4_rows:
            plot_bar_chart(
                experiment4_rows,
                baseline_avg_ns,
                title="Experiment4: parent train, child trigger",
                path=experiment4_plot_path(),
            )
