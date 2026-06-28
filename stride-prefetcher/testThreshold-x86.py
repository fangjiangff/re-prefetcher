import argparse
import csv
import os
import subprocess
import sys


SRC = "triggerThreshold-x86.cc"
OUT = "bin/triggerThreshold-x86"

# ARCHES = ["RaptorCove", "Gracemont"]
ARCHES = ["RC", "GM","CL","Zen4"]
CORES = [0, 16, 0, 0]
if len(ARCHES) != len(CORES):
    raise ValueError("ARCHES and CORES must have the same length")

ARCH_CORE_MAP = dict(zip(ARCHES, CORES))
DEFAULT_ARCH = ARCHES[0]
DEFAULT_STRIDE_LINES = 11
DEFAULT_TRAIN_STEP = 15
DEFAULT_ROUNDS = 4000
DEFAULT_PROBE_POSITIONS = 200
DEFAULT_HIT_THRESHOLD_CYCLES = 120

MODES = {
    "load": {"TEST_ON_ST": 0, "TEST_ON_SW": 0, "title": "Load instruction"},
    "store": {"TEST_ON_ST": 1, "TEST_ON_SW": 0, "title": "Store instruction"},
    "sw": {"TEST_ON_ST": 0, "TEST_ON_SW": 1, "title": "Software prefetch"},
}

ROLE_COLORS = {
    "accessed": "#D55E00",
    "prefetched": "#0072B2",
    "cache_miss": "#BDBDBD",
}


def parse_args():
    parser = argparse.ArgumentParser()
    parser.add_argument("--stride", type=int, default=DEFAULT_STRIDE_LINES,
                        help=f"Stride in cache lines. Default: {DEFAULT_STRIDE_LINES}")
    parser.add_argument("--train-step", type=int, default=DEFAULT_TRAIN_STEP,
                        help=f"Number of accessed positions. Default: {DEFAULT_TRAIN_STEP}")
    parser.add_argument("--rounds", type=int, default=DEFAULT_ROUNDS,
                        help=f"Rounds per mode. Default: {DEFAULT_ROUNDS}")
    parser.add_argument("--probe-positions", type=int, default=DEFAULT_PROBE_POSITIONS,
                        help=f"Positions to probe. Default: {DEFAULT_PROBE_POSITIONS}")
    parser.add_argument("--hit-threshold-cycles", type=int,
                        default=DEFAULT_HIT_THRESHOLD_CYCLES,
                        help="Average latency threshold used by Python to classify "
                             "non-accessed positions as prefetched. "
                             f"Default: {DEFAULT_HIT_THRESHOLD_CYCLES}")
    parser.add_argument("--mode", choices=["both", "load","store", "sw"], default="both",
                        help="Access mode to run. Default: both")
    parser.add_argument("--core", type=int, default=None,
                        help="Override CPU core used by taskset. Default is selected from --arch.")
    parser.add_argument("--arch", default=DEFAULT_ARCH, choices=ARCHES,
                        help=f"Architecture to run. Choices: {', '.join(ARCHES)}. "
                             f"Default: {DEFAULT_ARCH}")
    parser.add_argument("--plot-only", action="store_true",
                        help="Only plot from the existing TSV result.")
    args = parser.parse_args()

    if args.core is None:
        args.core = ARCH_CORE_MAP[args.arch]
    if args.core < 0:
        parser.error("--core must be >= 0")
    if args.stride < 1:
        parser.error("--stride must be >= 1")
    if args.train_step < 1:
        parser.error("--train-step must be >= 1")
    if args.rounds < 1:
        parser.error("--rounds must be >= 1")
    if args.probe_positions < 1:
        parser.error("--probe-positions must be >= 1")
    if args.hit_threshold_cycles < 1:
        parser.error("--hit-threshold-cycles must be >= 1")
    return args


args = parse_args()
stride_bytes = args.stride * 64
mode_names = ["load", "store", "sw"] if args.mode == "both" else [args.mode]
accessed_offsets = {step * stride_bytes for step in range(args.train_step)}
micro_arch = (
    f"{args.arch}-core{args.core}-stride{args.stride}"
    f"-train{args.train_step}-probe{args.probe_positions}"
)
result_dir = os.path.join("res", "x86-role")
plot_dir = os.path.join("res", "barplots")
raw_dir = os.path.join(result_dir, "raw")
tsv_path = os.path.join(result_dir, f"{micro_arch}.tsv")
plot_path = os.path.join(plot_dir, f"{micro_arch}-avg_cycles.png")


def ensure_dirs():
    os.makedirs(result_dir, exist_ok=True)
    os.makedirs(plot_dir, exist_ok=True)
    os.makedirs(raw_dir, exist_ok=True)


def parse_output(mode_name, output):
    rows = []
    for line in output.splitlines():
        stripped = line.strip()
        if not stripped or stripped.startswith("#"):
            continue

        fields = stripped.split()
        if len(fields) != 4:
            print(f"Skipping unexpected output row: {line}", file=sys.stderr)
            continue

        position, offset_bytes, avg_cycles, probes = fields
        try:
            position = int(position)
            avg_cycles = int(avg_cycles)
            probes = int(probes)
            rows.append({
                "mode": mode_name,
                "position": position,
                "offset_bytes": int(offset_bytes),
                "role": classify_position(int(offset_bytes), avg_cycles, probes),
                "avg_cycles": avg_cycles,
                "probes": probes,
            })
        except ValueError:
            print(f"Skipping non-numeric output row: {line}", file=sys.stderr)
    return rows


def classify_position(offset_bytes, avg_cycles, probes):
    if offset_bytes in accessed_offsets:
        return "accessed"
    if probes <= 0:
        return "cache_miss"
    if avg_cycles <= args.hit_threshold_cycles:
        return "prefetched"
    return "cache_miss"


def write_tsv(rows):
    fieldnames = ["mode", "position", "offset_bytes", "role",
                  "avg_cycles", "probes"]
    with open(tsv_path, "w", newline="") as f:
        writer = csv.DictWriter(f, fieldnames=fieldnames, delimiter="\t")
        writer.writeheader()
        writer.writerows(rows)


def read_tsv():
    rows = []
    with open(tsv_path, newline="") as f:
        reader = csv.DictReader(f, delimiter="\t")
        for row in reader:
            rows.append({
                "mode": row["mode"],
                "position": int(row["position"]),
                "offset_bytes": int(row["offset_bytes"]),
                "role": row["role"],
                "avg_cycles": int(row["avg_cycles"]),
                "probes": int(row["probes"]),
            })
    return rows


def run_tests():
    all_rows = []
    for mode_name in mode_names:
        config = MODES[mode_name]
        print("=" * 60)
        print(
            f"mode={mode_name}, arch={args.arch}, core={args.core}, "
            f"stride={args.stride} lines, train_step={args.train_step}, "
            f"probe_positions={args.probe_positions}, rounds={args.rounds}"
        )

        compile_cmd = [
            "g++",
            "-std=gnu11",
            "-O0",
            "-static",
            f"-DTEST_ON_SW={config['TEST_ON_SW']}",
            f"-DTEST_ON_ST={config['TEST_ON_ST']}",
            f"-DSTRIDE_BYTES={stride_bytes}",
            f"-DTRAIN_STEP={args.train_step}",
            f"-DROUNDS={args.rounds}",
            f"-DPROBE_POSITIONS={args.probe_positions}",
            f"-DCPU_ID={args.core}",
            "-o",
            OUT,
            SRC,
        ]

        compiled = subprocess.run(compile_cmd)
        if compiled.returncode != 0:
            print("Compile failed")
            continue

        run = subprocess.run(
            ["taskset", "-c", str(args.core), "./" + OUT],
            capture_output=True,
            text=True,
        )
        if run.returncode != 0:
            print("Execution failed")
            if run.stderr:
                print(run.stderr)
            continue

        raw_path = os.path.join(raw_dir, f"{micro_arch}-{mode_name}.txt")
        with open(raw_path, "w") as f:
            f.write(run.stdout)
        print(f"Saved raw output to {raw_path}")

        rows = parse_output(mode_name, run.stdout)
        if not rows:
            print("No parseable result rows")
            continue
        all_rows.extend(rows)

    if not all_rows:
        print("No results to save or plot")
        return []

    write_tsv(all_rows)
    print(f"Saved parsed results to {tsv_path}")
    return all_rows


def plot_bar_chart(rows):
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

    modes = [mode for mode in mode_names if any(row["mode"] == mode for row in rows)]
    if not modes:
        print("No rows for selected modes")
        return

    width = min(max(args.probe_positions / 10, 10), 28)
    fig, axes = plt.subplots(len(modes), 1, figsize=(width, 4 * len(modes)), sharex=True)
    if len(modes) == 1:
        axes = [axes]

    for ax, mode_name in zip(axes, modes):
        mode_rows = sorted(
            [row for row in rows if row["mode"] == mode_name],
            key=lambda row: row["position"],
        )
        positions = [row["position"] for row in mode_rows]
        values = [row["avg_cycles"] for row in mode_rows]
        colors = [ROLE_COLORS.get(row["role"], ROLE_COLORS["cache_miss"]) for row in mode_rows]

        ax.bar(positions, values, color=colors, width=0.85, edgecolor="black", linewidth=0.25)
        ax.axhline(args.hit_threshold_cycles, color="black", linestyle="--", linewidth=0.9)
        ax.set_title(MODES[mode_name]["title"], loc="left", pad=4)
        ax.set_ylabel("Average reload cycles")
        ax.set_ylim(0, 300)
        ax.grid(axis="y", alpha=0.25)
        ax.set_xlim(-1, max(positions) + 1 if positions else args.probe_positions)

    tick_step = max(1, args.probe_positions // 16)
    axes[-1].set_xticks(range(0, args.probe_positions, tick_step))
    axes[-1].set_xlabel("Probe cache-line index")

    legend_items = [
        Patch(facecolor=ROLE_COLORS["accessed"], edgecolor="black", label="accessed"),
        Patch(facecolor=ROLE_COLORS["prefetched"], edgecolor="black", label="prefetched"),
        Patch(facecolor=ROLE_COLORS["cache_miss"], edgecolor="black", label="cache_miss"),
    ]
    axes[0].legend(handles=legend_items, loc="upper right", frameon=False, ncol=3)

    fig.suptitle(
        f"{args.arch} core {args.core}, stride={args.stride} lines, "
        f"train_step={args.train_step}, hit_threshold={args.hit_threshold_cycles} cycles",
        x=0.01,
        ha="left",
    )
    fig.tight_layout(rect=(0, 0, 1, 0.95))
    plt.savefig(plot_path, dpi=300)
    plt.close(fig)
    print(f"Saved bar chart to {plot_path}")


if __name__ == "__main__":
    ensure_dirs()
    if args.plot_only:
        if not os.path.exists(tsv_path):
            print(f"Error: TSV result '{tsv_path}' not found.")
            sys.exit(1)
        result_rows = read_tsv()
    else:
        result_rows = run_tests()
    if result_rows:
        plot_bar_chart(result_rows)
