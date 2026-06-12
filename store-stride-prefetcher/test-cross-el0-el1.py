import argparse
import csv
import os
import subprocess
import sys


BASE_DIR = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.join(BASE_DIR, "test-cross-el0-el1.c")
UTIL_SRC = os.path.join(BASE_DIR, "until.c")
OUT = os.path.join(BASE_DIR, "bin", "test-cross-el0-el1")

DEFAULT_ARCH = "A55"
DEFAULT_CORE = 1
DEFAULT_STRIDE_LINES = 5
DEFAULT_TRAIN_ACCESSES = 5
DEFAULT_ROUNDS = 4000
DEFAULT_PROBE_POSITIONS = 64
DEFAULT_HIT_THRESHOLD_NS = 120

ROLE_COLORS = {
    "trained": "#D55E00",
    "trigger": "#CC79A7",
    "predicted": "#0072B2",
    "prefetched": "#56B4E9",
    "cache_miss": "#BDBDBD",
}


def parse_args():
    parser = argparse.ArgumentParser(
        description="Compile, run, and plot EL0/EL1 stride test."
    )
    parser.add_argument("--arch", default=DEFAULT_ARCH)
    parser.add_argument("--core", type=int, default=DEFAULT_CORE)
    parser.add_argument("--stride", type=int, default=DEFAULT_STRIDE_LINES,
                        help="Stride in cache lines. Default: 5")
    parser.add_argument("--train-accesses", type=int,
                        default=DEFAULT_TRAIN_ACCESSES,
                        help="EL0 stores before the EL1 syscall trigger.")
    parser.add_argument("--rounds", type=int, default=DEFAULT_ROUNDS)
    parser.add_argument("--probe-positions", type=int,
                        default=DEFAULT_PROBE_POSITIONS)
    parser.add_argument("--hit-threshold-ns", type=int,
                        default=DEFAULT_HIT_THRESHOLD_NS)
    parser.add_argument("--access", choices=["store", "load"], default="store",
                        help="Stride instruction to test. Default: store")
    parser.add_argument("--inline-store", action="store_true",
                        help="Use inline EL0 training stores instead of "
                             "noinline same-PC stores.")
    parser.add_argument("--cc", default=os.environ.get("CC", "gcc"))
    parser.add_argument("--plot-only", action="store_true")
    parser.add_argument("--no-plot", action="store_true")
    args = parser.parse_args()

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
result_dir = os.path.join(BASE_DIR, "res", "cross-el0-el1")
plot_dir = os.path.join(BASE_DIR, "res", "barplots")
raw_dir = os.path.join(result_dir, "raw")


def train_only_accesses(parsed_args=args):
    if parsed_args.access == "load":
        return parsed_args.train_accesses - 1
    return parsed_args.train_accesses


def trigger_line_for_args(parsed_args=args):
    if parsed_args.access == "load":
        return (parsed_args.train_accesses - 1) * parsed_args.stride
    return parsed_args.train_accesses * parsed_args.stride


def predicted_line_for_args(parsed_args=args):
    return trigger_line_for_args(parsed_args) + parsed_args.stride


def micro_arch_name():
    if args.access == "load":
        pc_mode = "noinline"
    else:
        pc_mode = "inline" if args.inline_store else "noinline"
    return (
        f"{args.arch}-core{args.core}-cross-el"
        f"-stride{args.stride}-train{args.train_accesses}"
        f"-{args.access}-{pc_mode}"
    )


def tsv_path():
    return os.path.join(result_dir, f"{micro_arch_name()}.tsv")


def plot_path():
    return os.path.join(plot_dir, f"{micro_arch_name()}.png")


def same_el0_plot_path():
    return os.path.join(plot_dir, f"{micro_arch_name()}-same-el0-baseline.png")


def control_tsv_path():
    return os.path.join(result_dir, f"{micro_arch_name()}-no-trigger-control.tsv")


def control_raw_path():
    return os.path.join(raw_dir, f"{micro_arch_name()}-no-trigger-control.txt")


def same_el0_tsv_path():
    return os.path.join(result_dir, f"{micro_arch_name()}-same-el0-baseline.tsv")


def same_el0_raw_path():
    return os.path.join(raw_dir, f"{micro_arch_name()}-same-el0-baseline.txt")


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


def compile_test(no_trigger=False, same_el0=False):
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
        f"-DSAME_EL0_TRIGGER={1 if same_el0 else 0}",
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

    rows = parse_output(run.stdout)
    if not rows:
        print("No-trigger control produced no rows")
        return None

    write_tsv(rows, control_tsv_path())
    print_summary(rows, "No-trigger result:")
    predicted_line = predicted_line_for_args()
    for row in rows:
        if row["position"] == predicted_line:
            print(
                f"No-trigger control: line {predicted_line} "
                f"avg_ns={row['avg_ns']}"
            )
            return row["avg_ns"]

    print(f"No-trigger control missing line {predicted_line}")
    return None


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
    print(f"Saved same-EL0 raw output to {same_el0_raw_path()}")
    print(f"Saved same-EL0 parsed results to {same_el0_tsv_path()}")
    print_summary(rows, "Same-EL0 baseline result:")
    return rows


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
        f"EL0/EL1 {args.access}-stride, arch={args.arch}, core={args.core}, "
        f"stride={args.stride} lines, train_accesses={args.train_accesses}, "
        f"train_only_accesses={train_only_accesses()}, "
        f"trigger_line={trigger_line_for_args()}, "
        f"predicted_line={predicted_line_for_args()}, "
        f"probe_positions={args.probe_positions}, rounds={args.rounds}, "
        f"EL0 {args.access}={'noinline' if args.access == 'load' else ('inline' if args.inline_store else 'noinline')}, "
        f"EL1 trigger={'write(/dev/null)' if args.access == 'load' else 'read(/dev/zero)'}"
    )

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
    print(f"Saved raw output to {raw_path}")

    rows = parse_output(run.stdout)
    if not rows:
        print("No results to save or plot")
        return []

    path = tsv_path()
    write_tsv(rows, path)
    print(f"Saved parsed results to {path}")
    print_summary(rows, "Trigger result:")
    return rows


def plot_bar_chart(rows, no_trigger_avg_ns=None, title=None, path=None):
    try:
        import matplotlib.pyplot as plt
        from matplotlib.patches import Patch
    except ModuleNotFoundError as exc:
        print(f"Skipping bar plot: missing Python package '{exc.name}'.")
        print("Install plotting dependencies with:")
        print("  sudo apt install python3-matplotlib")
        print("or, for the current Python environment:")
        print("  python3 -m pip install matplotlib")
        return

    sorted_rows = sorted(rows, key=lambda row: row["position"])
    positions = [row["position"] for row in sorted_rows]
    values = [row["avg_ns"] for row in sorted_rows]
    colors = [
        ROLE_COLORS.get(row["role"], ROLE_COLORS["cache_miss"])
        for row in sorted_rows
    ]

    width = min(max(args.probe_positions / 5, 10), 18)
    fig, ax = plt.subplots(1, 1, figsize=(width, 4))

    ax.bar(positions, values, color=colors, width=0.85,
           edgecolor="black", linewidth=0.25)
    ax.axhline(args.hit_threshold_ns, color="black",
               linestyle="--", linewidth=0.9)
    ax.axvline(trigger_line_for_args(), color="#CC79A7",
               linestyle=":", linewidth=1.0)
    ax.axvline(predicted_line_for_args(), color="#0072B2",
               linestyle=":", linewidth=1.0)

    if title is None:
        title = f"EL0 {args.access} train, EL1 {args.access} trigger"
    ax.set_title(title, loc="left", pad=4)
    ax.set_ylabel("Average reload ns")
    ax.set_xlabel("Probe cache-line index")
    ax.set_ylim(0, max(300, max(values) * 1.05 if values else 300))
    ax.set_xlim(-1, max(positions) + 1 if positions else args.probe_positions)
    ax.grid(axis="y", alpha=0.25)

    tick_step = max(1, args.probe_positions // 16)
    ax.set_xticks(range(0, args.probe_positions, tick_step))

    legend_items = [
        Patch(facecolor=ROLE_COLORS["trained"], edgecolor="black",
              label="EL0 trained"),
        Patch(facecolor=ROLE_COLORS["trigger"], edgecolor="black",
              label=f"{args.access} trigger"),
        Patch(facecolor=ROLE_COLORS["predicted"], edgecolor="black",
              label="predicted"),
        Patch(facecolor=ROLE_COLORS["prefetched"], edgecolor="black",
              label="other prefetched"),
        Patch(facecolor=ROLE_COLORS["cache_miss"], edgecolor="black",
              label="cache_miss"),
    ]
    ax.legend(handles=legend_items, loc="upper right", frameon=False, ncol=3)

    no_trigger_text = ""
    if no_trigger_avg_ns is not None:
        no_trigger_text = (
            f", no_trigger_line{predicted_line_for_args()}"
            f"={no_trigger_avg_ns} ns"
        )

    fig.suptitle(
        f"{args.arch} core {args.core}, stride={args.stride}, "
        f"train_only={train_only_accesses()}, trigger_accesses={args.train_accesses}, "
        f"EL0 {args.access}={'noinline' if args.access == 'load' else ('inline' if args.inline_store else 'noinline')}, "
        f"threshold={args.hit_threshold_ns} ns"
        f"{no_trigger_text}",
        x=0.01,
        ha="left",
    )
    fig.tight_layout(rect=(0, 0, 1, 0.94))
    if path is None:
        path = plot_path()
    fig.savefig(path, dpi=300)
    plt.close(fig)
    print(f"Saved bar chart to {path}")


if __name__ == "__main__":
    ensure_dirs()
    baseline_avg_ns = None
    if args.plot_only:
        path = tsv_path()
        if not os.path.exists(path):
            print(f"Error: TSV result '{path}' not found.")
            sys.exit(1)
        if os.path.exists(same_el0_tsv_path()):
            same_rows = read_tsv(same_el0_tsv_path())
            print_summary(same_rows, "Existing same-EL0 baseline result:")
            if not args.no_plot:
                plot_bar_chart(
                    same_rows,
                    None,
                    title=f"Same-EL0 {args.access} baseline",
                    path=same_el0_plot_path(),
                )
        if os.path.exists(control_tsv_path()):
            control_rows = read_tsv(control_tsv_path())
            baseline_avg_ns = control_rows[predicted_line_for_args()]["avg_ns"]
            print_summary(control_rows, "Existing no-trigger result:")
        result_rows = read_tsv(path)
        print_summary(result_rows, "Existing trigger result:")
    else:
        baseline_avg_ns = no_trigger_baseline()
        same_rows = run_same_el0_baseline()
        if same_rows and not args.no_plot:
            plot_bar_chart(
                same_rows,
                baseline_avg_ns,
                title=f"Same-EL0 {args.access} baseline",
                path=same_el0_plot_path(),
            )
        result_rows = run_test()

    if result_rows and not args.no_plot:
        plot_bar_chart(result_rows, baseline_avg_ns)
