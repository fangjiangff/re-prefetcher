import argparse
import csv
import os
import subprocess
import sys

ROOT_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
if ROOT_DIR not in sys.path:
    sys.path.insert(0, ROOT_DIR)

from cross_test_config import (
    ARCH_CONFIG,
    apply_single_core_defaults,
    apply_train_access_defaults,
    arch_choices,
)
from cross_plot import plot_cross_bar_chart
from cross_result_eval import (
    compute_cross_threshold,
    print_cross_evaluation,
    reclassify_cross_rows,
)

BASE_DIR = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.join(BASE_DIR, "test3-va-cross-el0-el1.c")
UTIL_SRC = os.path.join(ROOT_DIR, "until.c")
OUT = os.path.join(ROOT_DIR, "bin", "test3-va-cross-el0-el1")

DEFAULT_STRIDE_LINES = 5
DEFAULT_ROUNDS = 4000
DEFAULT_PROBE_POSITIONS = 64
DEFAULT_HIT_THRESHOLD_NS = 150
DEFAULT_VA_MAP_ADDR = "0x700000000"
DEFAULT_DUMMY_BUFFER_PAGES = 64
PREFETCH_CANDIDATE_LINES = 10
ANSI_GREEN = "\033[32m"
ANSI_RESET = "\033[0m"

def parse_int_auto(value):
    try:
        return int(value, 0)
    except ValueError as exc:
        raise argparse.ArgumentTypeError(str(exc)) from exc


def parse_args():
    parser = argparse.ArgumentParser(
        description="Compile, run, and plot same-VA EL0/EL1 stride test."
    )
    parser.add_argument("--arch", required=True, choices=arch_choices())
    parser.add_argument("--core", type=int, default=None,
                        help="Override CPU core. Default is selected from --arch.")
    parser.add_argument("--stride", type=int, default=DEFAULT_STRIDE_LINES,
                        help="Stride in cache lines. Default: 5")
    parser.add_argument("--train-accesses", type=int,
                        default=None,
                        help="Override trigger accesses. Default is selected "
                             "from --arch and --access.")
    parser.add_argument("--rounds", type=int, default=DEFAULT_ROUNDS)
    parser.add_argument("--probe-positions", type=int,
                        default=DEFAULT_PROBE_POSITIONS)
    parser.add_argument("--dummy-buffer-pages", type=int,
                        default=DEFAULT_DUMMY_BUFFER_PAGES,
                        help="Pages used by dummy and prefetcher-fill buffers. Default: 64")
    parser.add_argument("--hit-threshold-ns", type=int,
                        default=DEFAULT_HIT_THRESHOLD_NS)
    parser.add_argument("--access", choices=["store", "load"], default="store",
                        help="Stride instruction to test. Default: store")
    parser.add_argument("--timer",
                        choices=["gettime", "rdtsc", "cntvct", "pmccntr"],
                        default=None,
                        help="Timestamp source. Default is selected from --arch.")
    parser.add_argument("--pmu-device", default=None,
                        help="Override the PMU device name passed as PMU_DEVICE.")
    parser.add_argument("--va-map-addr", type=parse_int_auto,
                        default=parse_int_auto(DEFAULT_VA_MAP_ADDR),
                        help="Requested fixed EL0 user-page virtual address. Default: 0x700000000")
    parser.add_argument("--inline-store", action="store_true",
                        help="Use inline EL0 training stores instead of "
                             "noinline same-PC stores.")
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
    if args.train_accesses < 2:
        parser.error("--train-accesses must be >= 2")
    if args.rounds < 1:
        parser.error("--rounds must be >= 1")
    if args.probe_positions < 1 or args.probe_positions > 64:
        parser.error("--probe-positions must be in [1, 64]")
    if args.dummy_buffer_pages < 1:
        parser.error("--dummy-buffer-pages must be >= 1")
    if args.hit_threshold_ns < 1:
        parser.error("--hit-threshold-ns must be >= 1")
    if args.va_map_addr < 0:
        parser.error("--va-map-addr must be >= 0")

    predicted_line = args.train_accesses * args.stride
    if predicted_line >= 64:
        parser.error("train/trigger/predicted lines must fit in one 4KB page")

    return args


args = parse_args()
result_dir = os.path.join(ROOT_DIR, "res", "va-cross-el0-el1")
plot_dir = os.path.join(ROOT_DIR, "res", "barplots")
raw_dir = os.path.join(result_dir, "raw")


def train_only_accesses(parsed_args=args):
    return parsed_args.train_accesses - 1


def trigger_line_for_args(parsed_args=args):
    return (parsed_args.train_accesses - 1) * parsed_args.stride


def predicted_line_for_args(parsed_args=args):
    return trigger_line_for_args(parsed_args) + parsed_args.stride


def micro_arch_name():
    if args.access == "load":
        pc_mode = "noinline"
    else:
        pc_mode = "inline" if args.inline_store else "noinline"
    return (
        f"{args.arch}-core{args.core}-va-cross-el"
        f"-stride{args.stride}-train{args.train_accesses}"
        f"-{args.access}-{pc_mode}-sameva{args.va_map_addr:x}"
    )


def tsv_path():
    return os.path.join(result_dir, f"{micro_arch_name()}.tsv")


def plot_path():
    return os.path.join(plot_dir, f"{micro_arch_name()}.png")


def same_el0_plot_path():
    return os.path.join(plot_dir, f"{micro_arch_name()}-same-el0-baseline.png")


def context_switch_plot_path():
    return os.path.join(plot_dir, f"{micro_arch_name()}-context-switch-baseline.png")


def control_tsv_path():
    return os.path.join(result_dir, f"{micro_arch_name()}-no-trigger-control.tsv")


def control_raw_path():
    return os.path.join(raw_dir, f"{micro_arch_name()}-no-trigger-control.txt")


def same_el0_tsv_path():
    return os.path.join(result_dir, f"{micro_arch_name()}-same-el0-baseline.tsv")


def same_el0_raw_path():
    return os.path.join(raw_dir, f"{micro_arch_name()}-same-el0-baseline.txt")


def context_switch_tsv_path():
    return os.path.join(result_dir, f"{micro_arch_name()}-context-switch-baseline.tsv")


def context_switch_raw_path():
    return os.path.join(raw_dir, f"{micro_arch_name()}-context-switch-baseline.txt")


def context_switch_no_trigger_plot_path():
    return os.path.join(
        plot_dir, f"{micro_arch_name()}-context-switch-no-trigger.png"
    )


def context_switch_no_trigger_tsv_path():
    return os.path.join(
        result_dir, f"{micro_arch_name()}-context-switch-no-trigger.tsv"
    )


def context_switch_no_trigger_raw_path():
    return os.path.join(
        raw_dir, f"{micro_arch_name()}-context-switch-no-trigger.txt"
    )


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
    return trigger_line_for_args() * 64


def predicted_offset():
    return predicted_line_for_args() * 64


def timer_for_arch():
    if args.timer is not None:
        return args.timer
    return ARCH_CONFIG[args.arch]["timer"]


def timer_define():
    timer = timer_for_arch()
    if timer == "rdtsc":
        return "-DRDTSC=1"
    if timer == "cntvct":
        return "-DCNTVCT=1"
    if timer == "pmccntr":
        return "-DPMCCNTR=1"
    return "-DGETTIME=1"


def arch_cflags():
    if args.arch in ("X925", "A725"):
        return ["-march=armv8.5-a+predres"]
    return []


def pmu_defines():
    return [
        f"-DPMU_CORE_X925={1 if args.arch == 'X925' else 0}",
        f"-DPMU_CORE_A55={1 if args.arch == 'A55' else 0}",
    ]


def classify_position(offset_bytes, avg_ns, probes, no_trigger=False):
    if offset_bytes in trained_offsets():
        return "trained"
    if not no_trigger and offset_bytes == trigger_offset():
        return "trigger"
    if offset_bytes == predicted_offset():
        return "predicted"
    if probes > 0 and avg_ns <= args.hit_threshold_ns:
        return "prefetched"
    return "cache_miss"


def parse_output(output, no_trigger=False):
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
            "role": classify_position(
                offset_bytes, avg_ns, probes, no_trigger=no_trigger
            ),
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


def compile_test(no_trigger=False, same_el0=False, context_switch=False,
                 context_switch_no_trigger=False):
    use_noinline = 0 if args.inline_store else 1
    train_access_load = 1 if args.access == "load" else 0
    compile_cmd = [
        args.cc,
        "-std=gnu11",
        "-O0",
        "-static",
        *arch_cflags(),
        timer_define(),
        f"-DSTRIDE_LINES={args.stride}",
        f"-DTRAIN_ACCESSES={args.train_accesses}",
        f"-DROUNDS={args.rounds}",
        f"-DPROBE_POSITIONS={args.probe_positions}",
        f"-DDUMMY_BUFFER_PAGES={args.dummy_buffer_pages}",
        f"-DCPU_ID={args.core}",
        f"-DUSE_NOINLINE_STORE={use_noinline}",
        f"-DTRAIN_ACCESS_LOAD={train_access_load}",
        f"-DNO_TRIGGER={1 if no_trigger else 0}",
        f"-DSAME_EL0_TRIGGER={1 if same_el0 else 0}",
        f"-DCONTEXT_SWITCH_ONLY={1 if context_switch else 0}",
        f"-DCONTEXT_SWITCH_NO_TRIGGER={1 if context_switch_no_trigger else 0}",
        f"-DENABLE_CPP_RCTX={1 if args.arch in ('X925', 'A725') else 0}",
        *pmu_defines(),
        f"-DVA_MAP_ADDR=0x{args.va_map_addr:x}ULL",
        "-o",
        OUT,
        SRC,
        UTIL_SRC,
    ]
    return subprocess.run(compile_cmd)


def run_binary():
    env = os.environ.copy()
    if args.pmu_device is not None:
        env["PMU_DEVICE"] = args.pmu_device
    return subprocess.run(
        ["taskset", "-c", str(args.core), OUT],
        capture_output=True,
        text=True,
        env=env,
    )


def print_pmu_output(output):
    for line in output.splitlines():
        stripped = line.lstrip()
        if stripped.startswith("# PMU") or stripped.startswith("PMU:"):
            print(line)


def print_green(text):
    print(f"{ANSI_GREEN}{text}{ANSI_RESET}")


def effective_threshold_and_reclassify(*row_sets):
    threshold_ns, threshold_source = compute_cross_threshold(
        configured_threshold_ns=args.hit_threshold_ns,
        auto_threshold=True,
        baseline1_rows=row_sets[0] if len(row_sets) > 0 else None,
        baseline2_rows=row_sets[1] if len(row_sets) > 1 else None,
    )
    for rows in row_sets:
        reclassify_cross_rows(rows, threshold_ns)
    return threshold_ns, threshold_source


def no_trigger_baseline():
    compiled = compile_test(no_trigger=True)
    if compiled.returncode != 0:
        print("No-trigger control compile failed")
        return None

    run = run_binary()
    if run.returncode != 0:
        print("No-trigger control execution failed")
        if run.stdout:
            print(run.stdout)
        if run.stderr:
            print(run.stderr)
        return None

    with open(control_raw_path(), "w") as f:
        f.write(run.stdout)

    rows = parse_output(run.stdout, no_trigger=True)
    if not rows:
        print("No-trigger control produced no rows")
        return None

    write_tsv(rows, control_tsv_path())
    predicted_line = predicted_line_for_args()
    print()
    print_green(
        f"Baseline2 no-trigger: line {predicted_line} "
        f"avg_ns={rows[predicted_line]['avg_ns']}"
    )
    print_summary(rows, "No-trigger result:")
    print_pmu_output(run.stdout)
    print_pmu_output(run.stderr)
    return rows


def run_context_switch_baseline():
    compiled = compile_test(no_trigger=False, context_switch=True)
    if compiled.returncode != 0:
        print("Context-switch baseline compile failed")
        return []

    run = run_binary()
    if run.returncode != 0:
        print("Context-switch baseline execution failed")
        if run.stdout:
            print(run.stdout)
        if run.stderr:
            print(run.stderr)
        return []

    with open(context_switch_raw_path(), "w") as f:
        f.write(run.stdout)

    rows = parse_output(run.stdout)
    if not rows:
        print("Context-switch baseline produced no rows")
        return []

    write_tsv(rows, context_switch_tsv_path())
    predicted_line = predicted_line_for_args()
    print()
    print_green(
        f"Experiment3 EL1-switch EL0-trigger: line {predicted_line} "
        f"avg_ns={rows[predicted_line]['avg_ns']}"
    )
    print_summary(rows, "Context-switch baseline result:")
    print_pmu_output(run.stdout)
    print_pmu_output(run.stderr)
    return rows


def run_context_switch_no_trigger():
    compiled = compile_test(context_switch_no_trigger=True)
    if compiled.returncode != 0:
        print("Context-switch no-trigger compile failed")
        return []

    run = run_binary()
    if run.returncode != 0:
        print("Context-switch no-trigger execution failed")
        if run.stdout:
            print(run.stdout)
        if run.stderr:
            print(run.stderr)
        return []

    with open(context_switch_no_trigger_raw_path(), "w") as f:
        f.write(run.stdout)

    rows = parse_output(run.stdout, no_trigger=True)
    if not rows:
        print("Context-switch no-trigger produced no rows")
        return []

    write_tsv(rows, context_switch_no_trigger_tsv_path())
    predicted_line = predicted_line_for_args()
    print()
    print_green(
        f"Experiment5 EL1-switch no-trigger: line {predicted_line} "
        f"avg_ns={rows[predicted_line]['avg_ns']}"
    )
    print_summary(rows, "Context-switch no-trigger result:")
    print_pmu_output(run.stdout)
    print_pmu_output(run.stderr)
    return rows


def run_same_el0_baseline():
    compiled = compile_test(no_trigger=False, same_el0=True)
    if compiled.returncode != 0:
        print("Same-EL0 baseline compile failed")
        return []

    run = run_binary()
    if run.returncode != 0:
        print("Same-EL0 baseline execution failed")
        if run.stdout:
            print(run.stdout)
        if run.stderr:
            print(run.stderr)
        return []

    with open(same_el0_raw_path(), "w") as f:
        f.write(run.stdout)

    rows = parse_output(run.stdout)
    if not rows:
        print("Same-EL0 baseline produced no rows")
        return []

    write_tsv(rows, same_el0_tsv_path())
    predicted_line = predicted_line_for_args()
    print()
    print_green(
        f"Baseline1 same-EL0 trigger: line {predicted_line} "
        f"avg_ns={rows[predicted_line]['avg_ns']}"
    )
    print_summary(rows, "Same-EL0 baseline result:")
    print_pmu_output(run.stdout)
    print_pmu_output(run.stderr)
    return rows


def print_summary(rows, label):
    targets = [
        step * args.stride
        for step in range(train_only_accesses())
        if step * args.stride < min(args.probe_positions, len(rows))
    ]
    trigger_line = trigger_line_for_args()
    if trigger_line < min(args.probe_positions, len(rows)):
        targets.append(trigger_line)

    print(label)
    for pos in targets:
        row = rows[pos]
        print(f"  line {pos:2d}: {row['role']:10s} {row['avg_ns']:4d} ns")

    candidate_positions = [
        predicted_line_for_args() + candidate_index * args.stride
        for candidate_index in range(PREFETCH_CANDIDATE_LINES)
        if predicted_line_for_args() + candidate_index * args.stride
        < min(args.probe_positions, len(rows))
    ]
    print(
        "  Candidate prefetch lines at stride intervals: "
        + ", ".join(str(pos) for pos in candidate_positions)
    )
    for pos in candidate_positions:
        row = rows[pos]
        print(f"    line {pos:2d}: {row['role']:10s} {row['avg_ns']:4d} ns")


def run_test():
    compiled = compile_test(no_trigger=False)
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

    raw_path = os.path.join(raw_dir, f"{micro_arch_name()}.txt")
    with open(raw_path, "w") as f:
        f.write(run.stdout)

    rows = parse_output(run.stdout)
    if not rows:
        print("No results to save or plot")
        return []

    path = tsv_path()
    write_tsv(rows, path)
    predicted_line = predicted_line_for_args()
    print()
    print_green(
        f"Experiment4 EL1-trigger: line {predicted_line} "
        f"avg_ns={rows[predicted_line]['avg_ns']}"
    )
    print_summary(rows, "Trigger result:")
    print_pmu_output(run.stdout)
    print_pmu_output(run.stderr)
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
        trigger_label = f"{args.access} trigger"
    plot_cross_bar_chart(
        rows,
        args=args,
        trigger_line=trigger_line_for_args(),
        predicted_line=predicted_line_for_args(),
        train_only_accesses=train_only_accesses(),
        default_title=f"Same-VA EL0 {args.access} train, EL1 {args.access} trigger",
        trained_label="EL0 trained",
        trigger_label=trigger_label,
        summary_text=(
            f"{args.arch} core {args.core}, stride={args.stride}, "
            f"train_only={train_only_accesses()}, "
            f"trigger_accesses={args.train_accesses}, "
            f"EL0 {args.access}={access_mode}, "
            f"same_va=0x{args.va_map_addr:x}"
        ),
        output_path=path,
        no_trigger_avg_ns=no_trigger_avg_ns,
        title=title,
        quiet=True,
    )


def print_run_config():
    print("=" * 60)
    print(
        f"arch={args.arch}, core={args.core}, access={args.access}, "
        f"stride={args.stride}, accesses={args.train_accesses}, "
        f"train_only={train_only_accesses()}, "
        f"trigger_line={trigger_line_for_args()}, "
        f"predicted_line={predicted_line_for_args()}, "
        f"rounds={args.rounds}, timer={timer_for_arch()}, "
        f"va=0x{args.va_map_addr:x}"
    )
    print()


def print_final_evaluation(same_rows, control_rows, context_rows,
                           result_rows, context_no_trigger_rows):
    threshold_ns, threshold_source = effective_threshold_and_reclassify(
        same_rows,
        control_rows,
        context_rows,
        result_rows,
        context_no_trigger_rows,
    )
    print_cross_evaluation(
        predicted_line=predicted_line_for_args(),
        threshold_ns=threshold_ns,
        threshold_source=threshold_source,
        baseline1_rows=same_rows,
        baseline2_rows=control_rows,
        experiment3_rows=context_rows,
        experiment4_rows=result_rows,
        experiment5_rows=context_no_trigger_rows,
        experiment5_control_description=(
            "Experiment5 controls for EL1 switch without trigger"
        ),
    )


def generate_plots(same_rows, control_rows, context_rows,
                   result_rows, context_no_trigger_rows):
    if args.no_plot:
        return
    baseline_avg_ns = (
        control_rows[predicted_line_for_args()]["avg_ns"]
        if control_rows else None
    )
    if same_rows:
        plot_bar_chart(
            same_rows, baseline_avg_ns,
            title=f"Same-EL0 {args.access} baseline",
            path=same_el0_plot_path(),
        )
    if context_rows:
        plot_bar_chart(
            context_rows, baseline_avg_ns,
            title=f"EL1 context-switch {args.access} baseline",
            path=context_switch_plot_path(),
            trigger_label=f"EL0 {args.access} trigger after EL1 context switch",
        )
    if result_rows:
        plot_bar_chart(result_rows, baseline_avg_ns)
    if context_no_trigger_rows:
        plot_bar_chart(
            context_no_trigger_rows, baseline_avg_ns,
            title=f"EL1 context switch, no {args.access} trigger",
            path=context_switch_no_trigger_plot_path(),
            trigger_label="no trigger after EL1 context switch",
        )


if __name__ == "__main__":
    ensure_dirs()
    print_run_config()

    if args.plot_only:
        required_paths = {
            "Baseline1": same_el0_tsv_path(),
            "Baseline2": control_tsv_path(),
            "Experiment3": context_switch_tsv_path(),
            "Experiment4": tsv_path(),
            "Experiment5": context_switch_no_trigger_tsv_path(),
        }
        missing = [
            f"{name}: {path}"
            for name, path in required_paths.items()
            if not os.path.exists(path)
        ]
        if missing:
            print("Error: missing TSV result(s):")
            for item in missing:
                print(f"  {item}")
            sys.exit(1)

        same_rows = read_tsv(same_el0_tsv_path())
        control_rows = read_tsv(control_tsv_path())
        context_rows = read_tsv(context_switch_tsv_path())
        result_rows = read_tsv(tsv_path())
        context_no_trigger_rows = read_tsv(
            context_switch_no_trigger_tsv_path()
        )

        print_summary(same_rows, "Baseline1 same-EL0 trigger:")
        print_summary(control_rows, "Baseline2 no-trigger:")
        print_summary(context_rows, "Experiment3 EL1-switch, EL0-trigger:")
        print_summary(result_rows, "Experiment4 EL1-trigger:")
        print_summary(
            context_no_trigger_rows,
            "Experiment5 EL1-switch, no-trigger:",
        )
    else:
        same_rows = run_same_el0_baseline()
        control_rows = no_trigger_baseline()
        context_rows = run_context_switch_baseline()
        result_rows = run_test()
        context_no_trigger_rows = run_context_switch_no_trigger()

    print_final_evaluation(
        same_rows,
        control_rows,
        context_rows,
        result_rows,
        context_no_trigger_rows,
    )
    generate_plots(
        same_rows,
        control_rows,
        context_rows,
        result_rows,
        context_no_trigger_rows,
    )
