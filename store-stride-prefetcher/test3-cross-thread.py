import argparse
import csv
import os
import subprocess
import sys

from cross_test_config import (
    apply_access_defaults,
    apply_single_core_defaults,
    apply_threshold_defaults,
    arch_choices,
)
from cross_plot import plot_cross_bar_chart
from cross_result_eval import (
    compute_cross_threshold,
    print_cross_evaluation,
    reclassify_cross_rows,
)

BASE_DIR = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.join(BASE_DIR, "test3-cross-thread.c")
UTIL_SRC = os.path.join(BASE_DIR, "until.c")
OUT = os.path.join(BASE_DIR, "bin", "test3-cross-thread")

DEFAULT_STRIDE_LINES = 5
DEFAULT_ROUNDS = 4000
DEFAULT_PROBE_POSITIONS = 64
ANSI_GREEN = "\033[32m"
ANSI_RESET = "\033[0m"

def parse_args():
    parser = argparse.ArgumentParser(
        description="Compile, run, and plot cross-thread stride test."
    )
    parser.add_argument("--arch", required=True, choices=arch_choices())
    parser.add_argument("--core", type=int, default=None,
                        help="Override CPU core. Default is selected from --arch.")
    parser.add_argument("--stride", type=int, default=DEFAULT_STRIDE_LINES)
    parser.add_argument("--accesses", type=int,
                        default=None,
                        help="Total train+trigger accesses. "
                             "Default is selected from --arch and --access.")
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
    parser.add_argument("--access", choices=["store", "load"], default="store",
                        help="Stride instruction to test. Default: store")
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

    predicted_line = args.accesses * args.stride
    if predicted_line >= 64:
        parser.error("train/trigger/predicted lines must fit in one 4KB page")

    return args


args = parse_args()
result_dir = os.path.join(BASE_DIR, "res", "cross-thread")
plot_dir = os.path.join(BASE_DIR, "res", "barplots")
raw_dir = os.path.join(result_dir, "raw")


def train_only_accesses():
    return args.accesses - args.trigger_accesses


def trigger_line():
    return train_only_accesses() * args.stride


def last_trigger_line():
    return (args.accesses - 1) * args.stride


def predicted_line():
    return args.accesses * args.stride


def micro_arch_name():
    if args.access == "load":
        pc_mode = "noinline"
    else:
        pc_mode = "inline" if args.inline_store else "noinline"
    return (
        f"{args.arch}-core{args.core}-cross-thread"
        f"-stride{args.stride}-accesses{args.accesses}"
        f"-trigger{args.trigger_accesses}"
        f"-probe{args.probe_positions}-{args.access}-{pc_mode}"
    )


def tsv_path():
    return experiment4_tsv_path()


def raw_path():
    return experiment4_raw_path()


def plot_path():
    return experiment4_plot_path()


def baseline1_plot_path():
    return os.path.join(
        plot_dir, f"{micro_arch_name()}-baseline1-thread0-trigger-avg_ns.png"
    )


def baseline2_plot_path():
    return os.path.join(
        plot_dir, f"{micro_arch_name()}-baseline2-no-trigger-avg_ns.png"
    )


def experiment3_plot_path():
    return os.path.join(
        plot_dir, f"{micro_arch_name()}-exp3-thread1-context-thread0-trigger-avg_ns.png"
    )


def experiment4_plot_path():
    return os.path.join(
        plot_dir, f"{micro_arch_name()}-exp4-thread1-trigger-avg_ns.png"
    )


def experiment5_plot_path():
    return os.path.join(
        plot_dir, f"{micro_arch_name()}-exp5-thread1-context-no-trigger-avg_ns.png"
    )


def baseline1_tsv_path():
    return os.path.join(result_dir, f"{micro_arch_name()}-baseline1-thread0-trigger.tsv")


def baseline1_raw_path():
    return os.path.join(raw_dir, f"{micro_arch_name()}-baseline1-thread0-trigger.txt")


def baseline2_tsv_path():
    return os.path.join(result_dir, f"{micro_arch_name()}-baseline2-no-trigger.tsv")


def baseline2_raw_path():
    return os.path.join(raw_dir, f"{micro_arch_name()}-baseline2-no-trigger.txt")


def experiment3_tsv_path():
    return os.path.join(result_dir, f"{micro_arch_name()}-exp3-thread1-context-thread0-trigger.tsv")


def experiment3_raw_path():
    return os.path.join(raw_dir, f"{micro_arch_name()}-exp3-thread1-context-thread0-trigger.txt")


def experiment4_tsv_path():
    return os.path.join(result_dir, f"{micro_arch_name()}-exp4-thread1-trigger.tsv")


def experiment4_raw_path():
    return os.path.join(raw_dir, f"{micro_arch_name()}-exp4-thread1-trigger.txt")


def experiment5_tsv_path():
    return os.path.join(result_dir, f"{micro_arch_name()}-exp5-thread1-context-no-trigger.tsv")


def experiment5_raw_path():
    return os.path.join(raw_dir, f"{micro_arch_name()}-exp5-thread1-context-no-trigger.txt")


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


def trigger_offsets():
    return {
        (train_only_accesses() + trigger_index) * args.stride * 64
        for trigger_index in range(args.trigger_accesses)
    }


def predicted_offset():
    return predicted_line() * 64


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


def compile_test(no_trigger=False, context_switch=False, thread0_trigger=False):
    use_noinline = 0 if args.inline_store else 1
    train_access_load = 1 if args.access == "load" else 0
    compile_cmd = [
        args.cc,
        "-std=gnu11",
        "-O0",
        "-static",
        "-pthread",
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
        f"-DCONTEXT_SWITCH_ONLY={1 if context_switch else 0}",
        f"-DTHREAD0_TRIGGER={1 if thread0_trigger else 0}",
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


def result_paths(no_trigger=False, context_switch=False, thread0_trigger=False):
    if no_trigger and context_switch:
        return experiment5_raw_path(), experiment5_tsv_path()
    if no_trigger:
        return baseline2_raw_path(), baseline2_tsv_path()
    if context_switch:
        return experiment3_raw_path(), experiment3_tsv_path()
    if thread0_trigger:
        return baseline1_raw_path(), baseline1_tsv_path()
    return experiment4_raw_path(), experiment4_tsv_path()


def run_one(no_trigger=False, context_switch=False, thread0_trigger=False):
    compiled = compile_test(no_trigger=no_trigger,
                            context_switch=context_switch,
                            thread0_trigger=thread0_trigger)
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
        context_switch=context_switch,
        thread0_trigger=thread0_trigger,
    )
    path = raw_result_path
    with open(path, "w") as f:
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
            print(f"  line {pos:2d}: {row['role']:10s} {row['avg_ns']:4d} ns")


def print_green(text):
    print(f"{ANSI_GREEN}{text}{ANSI_RESET}")


def effective_threshold_and_reclassify(*row_sets):
    threshold_ns, threshold_source = compute_cross_threshold(
        configured_threshold_ns=args.hit_threshold_ns,
        auto_threshold=args.hit_threshold_ns_auto,
        baseline1_rows=row_sets[0] if len(row_sets) > 0 else None,
        baseline2_rows=row_sets[1] if len(row_sets) > 1 else None,
    )
    for rows in row_sets:
        reclassify_cross_rows(rows, threshold_ns)
    return threshold_ns, threshold_source


def run_test():
    print("=" * 60)
    print(
        f"cross-thread {args.access}-stride, arch={args.arch}, core={args.core}, "
        f"stride={args.stride} lines, accesses={args.accesses}, "
        f"train_only_accesses={train_only_accesses()}, "
        f"trigger_accesses={args.trigger_accesses}, "
        f"trigger_line={trigger_line()}, last_trigger_line={last_trigger_line()}, "
        f"predicted_line={predicted_line()}, "
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


def plot_bar_chart(rows, no_trigger_avg_ns=None, title=None, path=None,
                   trigger_label=None):
    access_mode = (
        "noinline" if args.access == "load"
        else ("inline" if args.inline_store else "noinline")
    )
    if path is None:
        path = plot_path()
    if trigger_label is None:
        trigger_label = f"thread1 {args.access} trigger"
    plot_cross_bar_chart(
        rows,
        args=args,
        trigger_line=trigger_line(),
        predicted_line=predicted_line(),
        train_only_accesses=train_only_accesses(),
        default_title=f"Thread0 train, thread1 {args.access} trigger",
        trained_label="thread0 trained",
        trigger_label=trigger_label,
        summary_text=(
            f"{args.arch} core {args.core}, stride={args.stride}, "
            f"accesses={args.accesses}, "
            f"train_only={train_only_accesses()}, "
            f"trigger_accesses={args.trigger_accesses}, "
            f"{args.access}={access_mode}"
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
        experiment5_rows = None
        if os.path.exists(baseline2_tsv_path()):
            baseline2_rows = read_tsv(baseline2_tsv_path())
            baseline_avg_ns = baseline2_rows[predicted_line()]["avg_ns"]
            print_summary(baseline2_rows, "Existing baseline2 no-trigger result:")
        if os.path.exists(baseline1_tsv_path()):
            baseline1_rows = read_tsv(baseline1_tsv_path())
            print_summary(baseline1_rows, "Existing baseline1 thread0-trigger result:")
            if not args.no_plot:
                plot_bar_chart(
                    baseline1_rows,
                    baseline_avg_ns,
                    title="Baseline1: thread0 train and trigger",
                    path=baseline1_plot_path(),
                    trigger_label=f"thread0 {args.access} trigger",
                )
        if os.path.exists(experiment3_tsv_path()):
            experiment3_rows = read_tsv(experiment3_tsv_path())
            print_summary(experiment3_rows, "Existing experiment3 context-switch result:")
            if not args.no_plot:
                plot_bar_chart(
                    experiment3_rows,
                    baseline_avg_ns,
                    title="Experiment3: thread1 context switch, thread0 trigger",
                    path=experiment3_plot_path(),
                    trigger_label=f"thread0 {args.access} trigger after thread1 context switch",
                )
        if os.path.exists(experiment4_tsv_path()):
            experiment4_rows = read_tsv(experiment4_tsv_path())
            print_summary(experiment4_rows, "Existing experiment4 thread1-trigger result:")
            if not args.no_plot:
                plot_bar_chart(
                    experiment4_rows,
                    baseline_avg_ns,
                    title="Experiment4: thread0 train, thread1 trigger",
                    path=experiment4_plot_path(),
                )
        if os.path.exists(experiment5_tsv_path()):
            experiment5_rows = read_tsv(experiment5_tsv_path())
            print_summary(experiment5_rows, "Existing experiment5 context-switch no-trigger result:")
            if not args.no_plot:
                plot_bar_chart(
                    experiment5_rows,
                    baseline_avg_ns,
                    title="Experiment5: thread1 context switch, no trigger",
                    path=experiment5_plot_path(),
                    trigger_label="no trigger after thread1 context switch",
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
        )
        if experiment5_rows:
            exp5_avg = experiment5_rows[predicted_line()]["avg_ns"]
            print(
                f"  Experiment5 context-switch no-trigger line {predicted_line}: "
                f"avg={exp5_avg} ns"
            )
        sys.exit(0)

    print("=" * 60)
    print(
        f"cross-thread {args.access}-stride, arch={args.arch}, core={args.core}, "
        f"stride={args.stride} lines, accesses={args.accesses}, "
        f"train_only_accesses={train_only_accesses()}, "
        f"trigger_accesses={args.trigger_accesses}, "
        f"trigger_line={trigger_line()}, last_trigger_line={last_trigger_line()}, "
        f"predicted_line={predicted_line()}, "
        f"probe_positions={args.probe_positions}, rounds={args.rounds}, "
        f"{args.access}={'noinline' if args.access == 'load' else ('inline' if args.inline_store else 'noinline')}"
    )

    baseline1_rows = run_one(thread0_trigger=True)
    if baseline1_rows:
        predicted = predicted_line()
        print_green(
            f"Baseline1 thread0-trigger: line {predicted} "
            f"avg_ns={baseline1_rows[predicted]['avg_ns']}"
        )
        print_summary(baseline1_rows, "Baseline1 thread0-trigger result:")

    baseline2_rows = run_one(no_trigger=True)
    if baseline2_rows:
        predicted = predicted_line()
        baseline_avg_ns = baseline2_rows[predicted]["avg_ns"]
        print_green(
            f"Baseline2 no-trigger: line {predicted} "
            f"avg_ns={baseline_avg_ns}"
        )
        print_summary(baseline2_rows, "Baseline2 no-trigger result:")

    experiment3_rows = run_one(context_switch=True)
    if experiment3_rows:
        predicted = predicted_line()
        print_green(
            f"Experiment3 context-switch thread0-trigger: line {predicted} "
            f"avg_ns={experiment3_rows[predicted]['avg_ns']}"
        )
        print_summary(experiment3_rows, "Experiment3 context-switch result:")

    experiment4_rows = run_one()
    if experiment4_rows:
        predicted = predicted_line()
        print_green(
            f"Experiment4 thread1-trigger: line {predicted} "
            f"avg_ns={experiment4_rows[predicted]['avg_ns']}"
        )
        print_summary(experiment4_rows, "Experiment4 thread1-trigger result:")

    experiment5_rows = run_one(no_trigger=True, context_switch=True)
    if experiment5_rows:
        predicted = predicted_line()
        print_green(
            f"Experiment5 context-switch no-trigger: line {predicted} "
            f"avg_ns={experiment5_rows[predicted]['avg_ns']}"
        )
        print_summary(experiment5_rows, "Experiment5 context-switch no-trigger result:")

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
    )
    if experiment5_rows:
        predicted = predicted_line()
        print(
            f"  Experiment5 context-switch no-trigger line {predicted}: "
            f"avg={experiment5_rows[predicted]['avg_ns']} ns"
        )

    if not args.no_plot:
        if baseline1_rows:
            plot_bar_chart(
                baseline1_rows,
                baseline_avg_ns,
                title="Baseline1: thread0 train and trigger",
                path=baseline1_plot_path(),
                trigger_label=f"thread0 {args.access} trigger",
            )
        if baseline2_rows:
            plot_bar_chart(
                baseline2_rows,
                None,
                title="Baseline2: thread0 train, no trigger",
                path=baseline2_plot_path(),
                trigger_label="no trigger",
            )
        if experiment3_rows:
            plot_bar_chart(
                experiment3_rows,
                baseline_avg_ns,
                title="Experiment3: thread1 context switch, thread0 trigger",
                path=experiment3_plot_path(),
                trigger_label=f"thread0 {args.access} trigger after thread1 context switch",
            )
        if experiment4_rows:
            plot_bar_chart(
                experiment4_rows,
                baseline_avg_ns,
                title="Experiment4: thread0 train, thread1 trigger",
                path=experiment4_plot_path(),
            )
        if experiment5_rows:
            plot_bar_chart(
                experiment5_rows,
                baseline_avg_ns,
                title="Experiment5: thread1 context switch, no trigger",
                path=experiment5_plot_path(),
                trigger_label="no trigger after thread1 context switch",
            )
