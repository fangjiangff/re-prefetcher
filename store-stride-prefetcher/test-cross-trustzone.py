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
SRC = os.path.join(BASE_DIR, "test-cross-trustzone.c")
UTIL_SRC = os.path.join(BASE_DIR, "until.c")
TA_DIR = os.path.join(BASE_DIR, "optee_store_stride_ta")
OUT = os.path.join(BASE_DIR, "bin", "test-cross-trustzone")

DEFAULT_STRIDE_LINES = 5
DEFAULT_ROUNDS = 4000
DEFAULT_PROBE_POSITIONS = 64
DEFAULT_HIT_THRESHOLD_NS = 150
DEFAULT_TA_UUID = "b6a189a0-7697-4aa8-9d62-80f64ec4e74d"
DEFAULT_TEE_DEVICE = "/dev/tee0"

def parse_args():
    parser = argparse.ArgumentParser(
        description="Compile, run, and plot Normal/Secure TrustZone stride test."
    )
    parser.add_argument("--arch", required=True, choices=arch_choices())
    parser.add_argument("--core", type=int, default=None,
                        help="Override CPU core. Default is selected from --arch.")
    parser.add_argument("--stride", type=int, default=DEFAULT_STRIDE_LINES)
    parser.add_argument("--train-accesses", type=int,
                        default=None,
                        help="Override trigger accesses. Default is selected "
                             "from --arch and --access.")
    parser.add_argument("--rounds", type=int, default=DEFAULT_ROUNDS)
    parser.add_argument("--probe-positions", type=int,
                        default=DEFAULT_PROBE_POSITIONS)
    parser.add_argument("--hit-threshold-ns", type=int,
                        default=DEFAULT_HIT_THRESHOLD_NS)
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
                        help="Skip the no-trigger baseline run.")
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
        trigger_line = (args.train_accesses - 1) * args.stride
    else:
        trigger_line = args.train_accesses * args.stride
    predicted_line = trigger_line + args.stride
    if predicted_line >= 64:
        parser.error("train/trigger/predicted lines must fit in one 4KB page")

    return args


args = parse_args()
result_dir = os.path.join(BASE_DIR, "res", "cross-trustzone")
plot_dir = os.path.join(BASE_DIR, "res", "barplots")
raw_dir = os.path.join(result_dir, "raw")


def train_only_count(parsed_args=args):
    if parsed_args.access == "load":
        return parsed_args.train_accesses - 1
    return parsed_args.train_accesses


def trigger_line_for(parsed_args=args):
    if parsed_args.access == "load":
        return (parsed_args.train_accesses - 1) * parsed_args.stride
    return parsed_args.train_accesses * parsed_args.stride


def predicted_line_for(parsed_args=args):
    return (trigger_line_for(parsed_args) + parsed_args.stride)


def micro_arch_name():
    if args.access == "load":
        pc_mode = "ns-samepc-secure-noop"
    else:
        pc_mode = "inline" if args.inline_store else "noinline"
    return (
        f"{args.arch}-core{args.core}-cross-trustzone"
        f"-stride{args.stride}-train{args.train_accesses}"
        f"-probe{args.probe_positions}-{args.access}-{pc_mode}"
    )


def pc_summary():
    if args.access == "load":
        return "NS load train+trigger=same noinline PC, secure=no-op"
    return f"normal store={'inline' if args.inline_store else 'noinline'}, secure store=noinline TA"


def tsv_path():
    return os.path.join(result_dir, f"{micro_arch_name()}.tsv")


def raw_path():
    return os.path.join(raw_dir, f"{micro_arch_name()}.txt")


def plot_path():
    return os.path.join(plot_dir, f"{micro_arch_name()}-avg_ns.png")


def no_secure_plot_path():
    return os.path.join(
        plot_dir,
        f"{micro_arch_name()}-no-secure-switch-baseline-avg_ns.png",
    )


def store_noop_plot_path():
    return os.path.join(
        plot_dir,
        f"{micro_arch_name()}-secure-noop-ns-trigger-avg_ns.png",
    )


def control_tsv_path():
    return os.path.join(result_dir, f"{micro_arch_name()}-no-trigger-control.tsv")


def control_raw_path():
    return os.path.join(raw_dir, f"{micro_arch_name()}-no-trigger-control.txt")


def no_secure_tsv_path():
    return os.path.join(result_dir, f"{micro_arch_name()}-no-secure-switch-baseline.tsv")


def no_secure_raw_path():
    return os.path.join(raw_dir, f"{micro_arch_name()}-no-secure-switch-baseline.txt")


def store_noop_tsv_path():
    return os.path.join(result_dir, f"{micro_arch_name()}-secure-noop-ns-trigger.tsv")


def store_noop_raw_path():
    return os.path.join(raw_dir, f"{micro_arch_name()}-secure-noop-ns-trigger.txt")


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


def predicted_offset():
    return predicted_line_for() * 64


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
        f"-DTRAIN_ACCESSES={args.train_accesses}",
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
    if ns_trigger_after_secure_noop:
        return store_noop_raw_path(), store_noop_tsv_path()
    if skip_secure_switch:
        return no_secure_raw_path(), no_secure_tsv_path()
    if no_trigger:
        return control_raw_path(), control_tsv_path()
    return raw_path(), tsv_path()


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
    targets = [0, 5, 10, 15, 20, 25, 30, 35, 40, 45]
    print(label)
    for pos in targets:
        if pos < len(rows):
            row = rows[pos]
            print(f"  line {pos:2d}: {row['role']:10s} {row['avg_ns']:4d} ns")


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
            f"train_only={train_only_count()}, "
            f"trigger_accesses={args.train_accesses}, "
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
        if not os.path.exists(tsv_path()):
            print(f"Error: TSV result '{tsv_path()}' not found.")
            sys.exit(1)
        rows = read_tsv(tsv_path())
        if os.path.exists(control_tsv_path()):
            control_rows = read_tsv(control_tsv_path())
            predicted_line = predicted_line_for()
            baseline_avg_ns = control_rows[predicted_line]["avg_ns"]
        if args.access == "load" and os.path.exists(no_secure_tsv_path()):
            no_secure_rows = read_tsv(no_secure_tsv_path())
            print_summary(no_secure_rows, "Existing no-secure-switch baseline:")
            if not args.no_plot:
                plot_bar_chart(
                    no_secure_rows,
                    baseline_avg_ns,
                    title="NS load train and trigger, no Secure World switch",
                    path=no_secure_plot_path(),
                )
        if args.access == "store" and os.path.exists(store_noop_tsv_path()):
            store_noop_rows = read_tsv(store_noop_tsv_path())
            print_summary(store_noop_rows, "Existing secure-noop NS-trigger result:")
            if not args.no_plot:
                plot_bar_chart(
                    store_noop_rows,
                    baseline_avg_ns,
                    title="NS store train, Secure World no-op, NS store trigger",
                    path=store_noop_plot_path(),
                )
        print_summary(rows, "Existing trigger result:")
        if not args.no_plot:
            plot_bar_chart(rows, baseline_avg_ns)
        sys.exit(0)

    print("=" * 60)
    print(
        f"TrustZone {args.access}-stride, arch={args.arch}, core={args.core}, "
        f"stride={args.stride} lines, train_accesses={args.train_accesses}, "
        f"train_only_accesses={train_only_count()}, "
        f"trigger_accesses={args.train_accesses}, "
        f"trigger_line={trigger_line_for()}, "
        f"predicted_line={predicted_line_for()}, "
        f"probe_positions={args.probe_positions}, rounds={args.rounds}, "
        f"{pc_summary()}, "
        f"ta_uuid={args.ta_uuid}, tee_device={args.tee_device}"
    )

    if not args.no_control:
        control_rows = run_one(no_trigger=True)
        if control_rows:
            predicted_line = predicted_line_for()
            baseline_avg_ns = control_rows[predicted_line]["avg_ns"]
            print(
                f"No-trigger control: line {predicted_line} "
                f"avg_ns={baseline_avg_ns}"
            )

    if args.access == "load":
        no_secure_rows = run_one(no_trigger=False, skip_secure_switch=True)
        if no_secure_rows:
            predicted_line = predicted_line_for()
            print(
                f"No-secure-switch baseline: line {predicted_line} "
                f"avg_ns={no_secure_rows[predicted_line]['avg_ns']}"
            )
            print(f"Saved no-secure-switch raw output to {no_secure_raw_path()}")
            print(f"Saved no-secure-switch parsed results to {no_secure_tsv_path()}")
            print_summary(no_secure_rows, "No-secure-switch baseline result:")
            if not args.no_plot:
                plot_bar_chart(
                    no_secure_rows,
                    baseline_avg_ns,
                    title="NS load train and trigger, no Secure World switch",
                    path=no_secure_plot_path(),
                )

    if args.access == "store":
        store_noop_rows = run_one(
            no_trigger=False,
            ns_trigger_after_secure_noop=True,
        )
        if store_noop_rows:
            predicted_line = predicted_line_for()
            print(
                f"Secure-noop NS-trigger store result: line {predicted_line} "
                f"avg_ns={store_noop_rows[predicted_line]['avg_ns']}"
            )
            print(f"Saved secure-noop NS-trigger raw output to {store_noop_raw_path()}")
            print(f"Saved secure-noop NS-trigger parsed results to {store_noop_tsv_path()}")
            print_summary(store_noop_rows, "Secure-noop NS-trigger store result:")
            if not args.no_plot:
                plot_bar_chart(
                    store_noop_rows,
                    baseline_avg_ns,
                    title="NS store train, Secure World no-op, NS store trigger",
                    path=store_noop_plot_path(),
                )

    rows = run_one(no_trigger=False)
    if rows:
        print(f"Saved raw output to {raw_path()}")
        print(f"Saved parsed results to {tsv_path()}")
        print_summary(rows, "Trigger result:")
        if not args.no_plot:
            plot_bar_chart(rows, baseline_avg_ns)
