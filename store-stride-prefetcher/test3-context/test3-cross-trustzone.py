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
)
from cross_plot import plot_cross_bar_chart
from cross_result_eval import (
    compute_cross_threshold,
    print_cross_evaluation,
    reclassify_cross_rows,
)

BASE_DIR = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.join(BASE_DIR, "test3-cross-trustzone.c")
UTIL_SRC = os.path.join(ROOT_DIR, "until.c")
TA_DIR = os.path.join(ROOT_DIR, "optee_store_stride_ta")
OUT = os.path.join(ROOT_DIR, "bin", "test3-cross-trustzone")

DEFAULT_STRIDE_LINES = 5
DEFAULT_ROUNDS = 4000
DEFAULT_PROBE_POSITIONS = 64
DEFAULT_TA_UUID = "b6a189a0-7697-4aa8-9d62-80f64ec4e74d"
DEFAULT_TEE_DEVICE = "/dev/tee0"
ANSI_GREEN = "\033[32m"
ANSI_RESET = "\033[0m"

def parse_args():
    parser = argparse.ArgumentParser(
        description="Compile, run, and plot Normal/Secure TrustZone stride test."
    )
    parser.add_argument("--arch", required=True, choices=arch_choices())
    parser.add_argument("--core", type=int, default=None,
                        help="Override CPU core. Default is selected from --arch.")
    parser.add_argument("--stride", type=int, default=DEFAULT_STRIDE_LINES)
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
    parser.add_argument("--access", choices=["store", "load"], default="store",
                        help="Instruction used for the stride sequence.")
    parser.add_argument("--inline-store", dest="inline_store", action="store_true",
                        help="Use inline Normal training accesses. Kept for compatibility.")
    parser.add_argument("--inline-access", dest="inline_store", action="store_true",
                        help="Use inline Normal training accesses.")
    parser.add_argument("--ta-uuid", default=DEFAULT_TA_UUID)
    parser.add_argument("--tee-device", default=DEFAULT_TEE_DEVICE)
    parser.add_argument("--cc", default=os.environ.get("CC", "gcc"))
    parser.add_argument("--build-ta", action="store_true",
                        help="Build the OP-TEE TA using $TA_DEV_KIT_DIR.")
    parser.add_argument("--ta-dev-kit-dir",
                        default=os.environ.get("TA_DEV_KIT_DIR"))
    parser.add_argument("--plot-only", action="store_true")
    parser.add_argument("--no-plot", action="store_true")
    parser.add_argument("--no-control", action="store_true",
                        help="Skip baseline2, the no-secure no-trigger run.")
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
result_dir = os.path.join(ROOT_DIR, "res", "cross-trustzone")
plot_dir = os.path.join(ROOT_DIR, "res", "barplots")
raw_dir = os.path.join(result_dir, "raw")


def train_only_count(parsed_args=args):
    return parsed_args.accesses - parsed_args.trigger_accesses


def first_trigger_line_for(parsed_args=args):
    return train_only_count(parsed_args) * parsed_args.stride


def last_trigger_line_for(parsed_args=args):
    return (parsed_args.accesses - 1) * parsed_args.stride


def second_trigger_line_for(parsed_args=args):
    return last_trigger_line_for(parsed_args)


def trigger_line_for(parsed_args=args):
    return first_trigger_line_for(parsed_args)


def predicted_line_for(parsed_args=args):
    return parsed_args.accesses * parsed_args.stride


def micro_arch_name():
    if args.access == "load":
        pc_mode = "ns-samepc-secure-noop"
    else:
        pc_mode = "inline" if args.inline_store else "noinline"
    return (
        f"{args.arch}-core{args.core}-cross-trustzone"
        f"-stride{args.stride}-accesses{args.accesses}"
        f"-trigger{args.trigger_accesses}"
        f"-probe{args.probe_positions}-{args.access}-{pc_mode}"
    )


def pc_summary():
    if args.access == "load":
        return "NS load train+trigger=same noinline PC, secure=no-op"
    return f"normal store={'inline' if args.inline_store else 'noinline'}, secure store=noinline TA"


def tsv_path():
    return experiment4_tsv_path()


def raw_path():
    return experiment4_raw_path()


def plot_path():
    return experiment4_plot_path()


def baseline1_plot_path():
    return os.path.join(
        plot_dir,
        f"{micro_arch_name()}-baseline1-no-secure-ns-trigger-avg_ns.png",
    )


def experiment3_plot_path():
    return os.path.join(
        plot_dir,
        f"{micro_arch_name()}-exp3-secure-noop-ns-trigger-avg_ns.png",
    )


def experiment4_plot_path():
    return os.path.join(
        plot_dir,
        f"{micro_arch_name()}-exp4-secure-trigger-avg_ns.png",
    )


def experiment5_plot_path():
    return os.path.join(
        plot_dir,
        f"{micro_arch_name()}-exp5-secure-noop-no-trigger-avg_ns.png",
    )


def baseline2_tsv_path():
    return os.path.join(result_dir, f"{micro_arch_name()}-baseline2-no-secure-no-trigger.tsv")


def baseline2_raw_path():
    return os.path.join(raw_dir, f"{micro_arch_name()}-baseline2-no-secure-no-trigger.txt")


def baseline1_tsv_path():
    return os.path.join(result_dir, f"{micro_arch_name()}-baseline1-no-secure-ns-trigger.tsv")


def baseline1_raw_path():
    return os.path.join(raw_dir, f"{micro_arch_name()}-baseline1-no-secure-ns-trigger.txt")


def experiment3_tsv_path():
    return os.path.join(result_dir, f"{micro_arch_name()}-exp3-secure-noop-ns-trigger.tsv")


def experiment3_raw_path():
    return os.path.join(raw_dir, f"{micro_arch_name()}-exp3-secure-noop-ns-trigger.txt")


def experiment4_tsv_path():
    return os.path.join(result_dir, f"{micro_arch_name()}-exp4-secure-trigger.tsv")


def experiment4_raw_path():
    return os.path.join(raw_dir, f"{micro_arch_name()}-exp4-secure-trigger.txt")


def experiment5_tsv_path():
    return os.path.join(result_dir, f"{micro_arch_name()}-exp5-secure-noop-no-trigger.tsv")


def experiment5_raw_path():
    return os.path.join(raw_dir, f"{micro_arch_name()}-exp5-secure-noop-no-trigger.txt")


def ensure_dirs():
    os.makedirs(os.path.dirname(OUT), exist_ok=True)
    os.makedirs(result_dir, exist_ok=True)
    os.makedirs(plot_dir, exist_ok=True)
    os.makedirs(raw_dir, exist_ok=True)


def trained_offsets():
    return {
        step * args.stride * 64 for step in range(train_only_count())
    }


def trigger_offset():
    return trigger_line_for() * 64


def trigger_offsets():
    return {
        (train_only_count() + trigger_index) * args.stride * 64
        for trigger_index in range(args.trigger_accesses)
    }


def predicted_offset():
    return predicted_line_for() * 64


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


def build_ta():
    if not args.ta_dev_kit_dir:
        print("TA_DEV_KIT_DIR is required for --build-ta", file=sys.stderr)
        return 1

    env = os.environ.copy()
    env["TA_DEV_KIT_DIR"] = args.ta_dev_kit_dir
    return subprocess.run(["make"], cwd=TA_DIR, env=env).returncode


def compile_host(no_trigger=False, skip_secure_switch=False,
                 ns_trigger_after_secure_noop=False):
    use_noinline = 0 if args.inline_store else 1
    train_access_load = 1 if args.access == "load" else 0
    compile_cmd = [
        args.cc,
        "-std=gnu11",
        "-O0",
        "-static",
        f"-DSTRIDE_LINES={args.stride}",
        f"-DTRAIN_ACCESSES={train_only_count()}",
        f"-DTRIGGER_ACCESSES={args.trigger_accesses}",
        f"-DROUNDS={args.rounds}",
        f"-DPROBE_POSITIONS={args.probe_positions}",
        f"-DCPU_ID={args.core}",
        f"-DUSE_NOINLINE_STORE={use_noinline}",
        f"-DTRAIN_ACCESS_LOAD={train_access_load}",
        f"-DNO_TRIGGER={1 if no_trigger else 0}",
        f"-DSKIP_SECURE_SWITCH={1 if skip_secure_switch else 0}",
        f"-DNS_TRIGGER_AFTER_SECURE_NOOP={1 if ns_trigger_after_secure_noop else 0}",
        f'-DDEFAULT_TA_UUID="{args.ta_uuid}"',
        "-o",
        OUT,
        SRC,
        UTIL_SRC,
    ]
    return subprocess.run(compile_cmd).returncode


def run_binary():
    return subprocess.run(
        ["taskset", "-c", str(args.core), OUT, args.ta_uuid, args.tee_device],
        capture_output=True,
        text=True,
    )


def result_paths(no_trigger=False, skip_secure_switch=False,
                 ns_trigger_after_secure_noop=False):
    if no_trigger and skip_secure_switch:
        return baseline2_raw_path(), baseline2_tsv_path()
    if no_trigger:
        return experiment5_raw_path(), experiment5_tsv_path()
    if ns_trigger_after_secure_noop:
        return experiment3_raw_path(), experiment3_tsv_path()
    if skip_secure_switch:
        return baseline1_raw_path(), baseline1_tsv_path()
    return experiment4_raw_path(), experiment4_tsv_path()


def run_one(no_trigger=False, skip_secure_switch=False,
            ns_trigger_after_secure_noop=False):
    if compile_host(no_trigger=no_trigger,
                    skip_secure_switch=skip_secure_switch,
                    ns_trigger_after_secure_noop=ns_trigger_after_secure_noop) != 0:
        print("Compile failed", file=sys.stderr)
        return None

    run = run_binary()
    if run.returncode != 0:
        print("Execution failed", file=sys.stderr)
        if run.stdout:
            print(run.stdout)
        if run.stderr:
            print(run.stderr, file=sys.stderr)
        return None

    raw_result_path, tsv_result_path = result_paths(
        no_trigger=no_trigger,
        skip_secure_switch=skip_secure_switch,
        ns_trigger_after_secure_noop=ns_trigger_after_secure_noop,
    )
    path = raw_result_path
    with open(path, "w") as f:
        f.write(run.stdout)

    rows = parse_output(run.stdout)
    if not rows:
        print("No parsed rows", file=sys.stderr)
        return None

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


def plot_bar_chart(rows, no_trigger_avg_ns=None, title=None, path=None):
    if title is None:
        if args.access == "load":
            title = "NS load train, Secure World no-op, NS load trigger"
        else:
            title = "Normal World store train, Secure World store trigger"
    if path is None:
        path = plot_path()
    plot_cross_bar_chart(
        rows,
        args=args,
        trigger_line=trigger_line_for(),
        predicted_line=predicted_line_for(),
        train_only_accesses=train_only_count(),
        default_title=title,
        trained_label=f"{args.access} trained",
        trigger_label=f"{args.access} trigger",
        summary_text=(
            f"{args.arch} core {args.core}, stride={args.stride}, "
            f"accesses={args.accesses}, "
            f"train_only={train_only_count()}, "
            f"trigger_accesses={args.trigger_accesses}, "
            f"{pc_summary()}"
        ),
        output_path=path,
        no_trigger_avg_ns=no_trigger_avg_ns,
        title=title,
    )


if __name__ == "__main__":
    ensure_dirs()
    baseline_avg_ns = None

    if args.build_ta:
        rc = build_ta()
        if rc != 0:
            sys.exit(rc)

    if args.plot_only:
        baseline1_rows = None
        baseline2_rows = None
        experiment3_rows = None
        experiment4_rows = None
        experiment5_rows = None
        if os.path.exists(baseline2_tsv_path()):
            baseline2_rows = read_tsv(baseline2_tsv_path())
            predicted_line = predicted_line_for()
            baseline_avg_ns = baseline2_rows[predicted_line]["avg_ns"]
            print_summary(baseline2_rows, "Existing baseline2 no-secure no-trigger:")
        if os.path.exists(baseline1_tsv_path()):
            baseline1_rows = read_tsv(baseline1_tsv_path())
            print_summary(baseline1_rows, "Existing baseline1 no-secure NS-trigger:")
            if not args.no_plot:
                plot_bar_chart(
                    baseline1_rows,
                    baseline_avg_ns,
                    title="Baseline1: NS train and trigger, no Secure World switch",
                    path=baseline1_plot_path(),
                )
        if os.path.exists(experiment3_tsv_path()):
            experiment3_rows = read_tsv(experiment3_tsv_path())
            print_summary(experiment3_rows, "Existing experiment3 secure-noop NS-trigger:")
            if not args.no_plot:
                plot_bar_chart(
                    experiment3_rows,
                    baseline_avg_ns,
                    title="Experiment3: Secure World no-op, NS train and trigger",
                    path=experiment3_plot_path(),
                )
        if os.path.exists(experiment4_tsv_path()):
            experiment4_rows = read_tsv(experiment4_tsv_path())
            print_summary(experiment4_rows, "Existing experiment4 secure-trigger:")
            if not args.no_plot:
                plot_bar_chart(
                    experiment4_rows,
                    baseline_avg_ns,
                    title="Experiment4: NS train, Secure World trigger",
                    path=experiment4_plot_path(),
                )
        if os.path.exists(experiment5_tsv_path()):
            experiment5_rows = read_tsv(experiment5_tsv_path())
            print_summary(experiment5_rows, "Existing experiment5 secure-noop no-trigger:")
            if not args.no_plot:
                plot_bar_chart(
                    experiment5_rows,
                    baseline_avg_ns,
                    title="Experiment5: NS train, Secure World no-op, no trigger",
                    path=experiment5_plot_path(),
                )
        threshold_ns, threshold_source = effective_threshold_and_reclassify(
            baseline1_rows,
            baseline2_rows,
            experiment3_rows,
            experiment4_rows,
            experiment5_rows,
        )
        print_cross_evaluation(
            predicted_line=predicted_line_for(),
            threshold_ns=threshold_ns,
            threshold_source=threshold_source,
            baseline1_rows=baseline1_rows,
            baseline2_rows=baseline2_rows,
            experiment3_rows=experiment3_rows,
            experiment4_rows=experiment4_rows,
            experiment5_rows=experiment5_rows,
        )
        sys.exit(0)

    print("=" * 60)
    print(
        f"TrustZone {args.access}-stride, arch={args.arch}, core={args.core}, "
        f"stride={args.stride} lines, accesses={args.accesses}, "
        f"train_only_accesses={train_only_count()}, "
        f"trigger_accesses={args.trigger_accesses}, "
        f"trigger_line={trigger_line_for()}, "
        f"last_trigger_line={last_trigger_line_for()}, "
        f"predicted_line={predicted_line_for()}, "
        f"probe_positions={args.probe_positions}, rounds={args.rounds}, "
        f"{pc_summary()}, "
        f"ta_uuid={args.ta_uuid}, tee_device={args.tee_device}"
    )

    baseline1_rows = run_one(no_trigger=False, skip_secure_switch=True)
    if baseline1_rows:
        predicted_line = predicted_line_for()
        print_green(
            f"Baseline1 no-secure NS-trigger: line {predicted_line} "
            f"avg_ns={baseline1_rows[predicted_line]['avg_ns']}"
        )
        # print(f"Saved baseline1 raw output to {baseline1_raw_path()}")
        # print(f"Saved baseline1 parsed results to {baseline1_tsv_path()}")
        print_summary(baseline1_rows, "Baseline1 no-secure NS-trigger result:")

    if not args.no_control:
        baseline2_rows = run_one(no_trigger=True, skip_secure_switch=True)
        if baseline2_rows:
            predicted_line = predicted_line_for()
            baseline_avg_ns = baseline2_rows[predicted_line]["avg_ns"]
            print_green(
                f"Baseline2 no-secure no-trigger: line {predicted_line} "
                f"avg_ns={baseline_avg_ns}"
            )
            # print(f"Saved baseline2 raw output to {baseline2_raw_path()}")
            # print(f"Saved baseline2 parsed results to {baseline2_tsv_path()}")
            print_summary(baseline2_rows, "Baseline2 no-secure no-trigger result:")
    else:
        baseline2_rows = None

    experiment3_rows = run_one(
        no_trigger=False,
        ns_trigger_after_secure_noop=True,
    )
    if experiment3_rows:
        predicted_line = predicted_line_for()
        print_green(
            f"Experiment3 secure-noop NS-trigger: line {predicted_line} "
            f"avg_ns={experiment3_rows[predicted_line]['avg_ns']}"
        )
        # print(f"Saved experiment3 raw output to {experiment3_raw_path()}")
        # print(f"Saved experiment3 parsed results to {experiment3_tsv_path()}")
        print_summary(experiment3_rows, "Experiment3 secure-noop NS-trigger result:")

    experiment5_rows = run_one(no_trigger=True)
    if experiment5_rows:
        predicted_line = predicted_line_for()
        print_green(
            f"Experiment5 secure-noop no-trigger: line {predicted_line} "
            f"avg_ns={experiment5_rows[predicted_line]['avg_ns']}"
        )
        # print(f"Saved experiment5 raw output to {experiment5_raw_path()}")
        # print(f"Saved experiment5 parsed results to {experiment5_tsv_path()}")
        print_summary(experiment5_rows, "Experiment5 secure-noop no-trigger result:")

    experiment4_rows = run_one(no_trigger=False)
    if experiment4_rows:
        predicted_line = predicted_line_for()
        print_green(
            f"Experiment4 secure-trigger: line {predicted_line} "
            f"avg_ns={experiment4_rows[predicted_line]['avg_ns']}"
        )
        # print(f"Saved experiment4 raw output to {experiment4_raw_path()}")
        # print(f"Saved experiment4 parsed results to {experiment4_tsv_path()}")
        print_summary(experiment4_rows, "Experiment4 secure-trigger result:")

    threshold_ns, threshold_source = effective_threshold_and_reclassify(
        baseline1_rows,
        baseline2_rows,
        experiment3_rows,
        experiment4_rows,
        experiment5_rows,
    )
    print_cross_evaluation(
        predicted_line=predicted_line_for(),
        threshold_ns=threshold_ns,
        threshold_source=threshold_source,
        baseline1_rows=baseline1_rows,
        baseline2_rows=baseline2_rows,
        experiment3_rows=experiment3_rows,
        experiment4_rows=experiment4_rows,
        experiment5_rows=experiment5_rows,
    )

    if not args.no_plot:
        if experiment4_rows:
            plot_bar_chart(
                experiment4_rows,
                baseline_avg_ns,
                title="Experiment4: NS train, Secure World trigger",
                path=experiment4_plot_path(),
            )
        if experiment3_rows:
            plot_bar_chart(
                experiment3_rows,
                baseline_avg_ns,
                title="Experiment3: Secure World no-op, NS train and trigger",
                path=experiment3_plot_path(),
            )
        if experiment5_rows:
            plot_bar_chart(
                experiment5_rows,
                baseline_avg_ns,
                title="Experiment5: NS train, Secure World no-op, no trigger",
                path=experiment5_plot_path(),
            )
        if baseline1_rows:
            plot_bar_chart(
                baseline1_rows,
                baseline_avg_ns,
                title="Baseline1: NS train and trigger, no Secure World switch",
                path=baseline1_plot_path(),
            )
