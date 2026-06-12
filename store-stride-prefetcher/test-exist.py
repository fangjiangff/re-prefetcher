import argparse
import csv
import os
import subprocess
import sys


BASE_DIR = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.join(BASE_DIR, "test-exist.c")
UTIL_SRC = os.path.join(BASE_DIR, "until.c")
OUT = os.path.join(BASE_DIR, "bin", "test-exist")

DEFAULT_ARCHES = ["A78", "A55", "A725", "X925", "A76"]
DEFAULT_CORES = [4, 1, 4, 6, 0]
DEFAULT_STRIDE_LINES = 5
DEFAULT_TRAIN_STEP = 6
DEFAULT_ROUNDS = 4000
DEFAULT_PROBE_POSITIONS = 200
DEFAULT_HIT_THRESHOLD_NS = 120

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
                        help=f"Rounds per test. Default: {DEFAULT_ROUNDS}")
    parser.add_argument("--probe-positions", type=int, default=DEFAULT_PROBE_POSITIONS,
                        help=f"Positions to probe. Default: {DEFAULT_PROBE_POSITIONS}")
    parser.add_argument("--hit-threshold-ns", type=int,
                        default=DEFAULT_HIT_THRESHOLD_NS,
                        help="Average latency threshold used by Python to classify "
                             "non-accessed positions as prefetched. "
                             f"Default: {DEFAULT_HIT_THRESHOLD_NS}")
    parser.add_argument("--access", choices=["store", "load"], default="store",
                        help="Stride instruction to test. Default: store")
    parser.add_argument("--no-trigger", action="store_true",
                        help="Skip the final same-PC trigger access after training.")
    parser.add_argument("--core", type=int, default=None,
                        help="CPU core used by taskset. If omitted, all default cores are tested.")
    parser.add_argument("--arch", default=None,
                        help="Architecture label for output files. If omitted, all default architectures are tested.")
    parser.add_argument("--cores", default=None,
                        help="Comma-separated CPU cores to test, paired with --arches.")
    parser.add_argument("--arches", default=None,
                        help="Comma-separated architecture labels to test, paired with --cores.")
    parser.add_argument("--cc", default=os.environ.get("CC", "gcc"),
                        help="Compiler command. Default: $CC or gcc")
    parser.add_argument("--plot-only", action="store_true",
                        help="Only plot from the existing TSV result.")
    args = parser.parse_args()

    if args.core is not None and args.core < 0:
        parser.error("--core must be >= 0")
    if args.stride < 1:
        parser.error("--stride must be >= 1")
    if args.train_step < 1:
        parser.error("--train-step must be >= 1")
    if args.rounds < 1:
        parser.error("--rounds must be >= 1")
    if args.probe_positions < 1:
        parser.error("--probe-positions must be >= 1")
    if args.hit_threshold_ns < 1:
        parser.error("--hit-threshold-ns must be >= 1")
    if (args.arch is None) != (args.core is None):
        parser.error("--arch and --core must be used together")
    if args.arches is not None and args.cores is None:
        parser.error("--arches requires --cores")
    if args.cores is not None and args.arches is None:
        parser.error("--cores requires --arches")
    if (args.arch is not None or args.core is not None) and (
        args.arches is not None or args.cores is not None
    ):
        parser.error("Use either --arch/--core or --arches/--cores, not both")
    return args


args = parse_args()
stride_bytes = args.stride * 64
accessed_offsets = {step * stride_bytes for step in range(args.train_step)}
result_dir = os.path.join(BASE_DIR, "res", "store-stride")
plot_dir = os.path.join(BASE_DIR, "res", "barplots")
raw_dir = os.path.join(result_dir, "raw")


def parse_csv_list(value):
    return [item.strip() for item in value.split(",") if item.strip()]


def get_targets():
    if args.arch is not None:
        return [(args.arch, args.core)]

    if args.arches is not None:
        arches = parse_csv_list(args.arches)
        try:
            cores = [int(core) for core in parse_csv_list(args.cores)]
        except ValueError:
            print("Error: --cores must contain comma-separated integers.", file=sys.stderr)
            sys.exit(2)
    else:
        arches = DEFAULT_ARCHES
        cores = DEFAULT_CORES

    if len(arches) != len(cores):
        print("Error: architecture and core lists must have the same length.", file=sys.stderr)
        sys.exit(2)
    if not arches:
        print("Error: at least one architecture/core pair is required.", file=sys.stderr)
        sys.exit(2)
    for core in cores:
        if core < 0:
            print("Error: cores must be >= 0.", file=sys.stderr)
            sys.exit(2)
    return list(zip(arches, cores))


def micro_arch_name(arch, core):
    trigger_suffix = "-no-trigger" if args.no_trigger else ""
    return (
        f"{arch}-core{core}-stride{args.stride}"
        f"-train{args.train_step}-{args.access}{trigger_suffix}"
    )


def tsv_path_for(arch, core):
    return os.path.join(result_dir, f"{micro_arch_name(arch, core)}.tsv")


def plot_path_for(arch, core):
    return os.path.join(plot_dir, f"{micro_arch_name(arch, core)}.png")


def ensure_dirs():
    os.makedirs(os.path.dirname(OUT), exist_ok=True)
    os.makedirs(result_dir, exist_ok=True)
    os.makedirs(plot_dir, exist_ok=True)
    os.makedirs(raw_dir, exist_ok=True)


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

        position, offset_bytes, avg_ns, probes = fields
        try:
            position = int(position)
            avg_ns = int(avg_ns)
            probes = int(probes)
            rows.append({
                "position": position,
                "offset_bytes": int(offset_bytes),
                "role": classify_position(int(offset_bytes), avg_ns, probes),
                "avg_ns": avg_ns,
                "probes": probes,
            })
        except ValueError:
            print(f"Skipping non-numeric output row: {line}", file=sys.stderr)
    return rows


def classify_position(offset_bytes, avg_ns, probes):
    if offset_bytes in accessed_offsets:
        return "accessed"
    if probes <= 0:
        return "cache_miss"
    if avg_ns <= args.hit_threshold_ns:
        return "prefetched"
    return "cache_miss"


def write_tsv(rows, tsv_path):
    fieldnames = ["position", "offset_bytes", "role", "avg_ns", "probes"]
    with open(tsv_path, "w", newline="") as f:
        writer = csv.DictWriter(f, fieldnames=fieldnames, delimiter="\t")
        writer.writeheader()
        writer.writerows(rows)


def read_tsv(tsv_path):
    rows = []
    with open(tsv_path, newline="") as f:
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


def run_tests(arch, core):
    print("=" * 60)
    print(
        f"access={args.access}, arch={arch}, core={core}, "
        f"stride={args.stride} lines, train_step={args.train_step}, "
        f"probe_positions={args.probe_positions}, rounds={args.rounds}"
    )

    compile_cmd = [
        args.cc,
        "-std=gnu11",
        "-O0",
        "-static",
        f"-DSTRIDE_BYTES={stride_bytes}",
        f"-DTRAIN_STEP={args.train_step}",
        f"-DROUNDS={args.rounds}",
        f"-DPROBE_POSITIONS={args.probe_positions}",
        f"-DCPU_ID={core}",
        f"-DTRAIN_ACCESS_LOAD={1 if args.access == 'load' else 0}",
        f"-DNO_TRIGGER={1 if args.no_trigger else 0}",
        "-o",
        OUT,
        SRC,
        UTIL_SRC,
    ]

    compiled = subprocess.run(compile_cmd)
    if compiled.returncode != 0:
        print("Compile failed")
        return []

    run = subprocess.run(
        ["taskset", "-c", str(core), OUT],
        capture_output=True,
        text=True,
    )
    if run.returncode != 0:
        print("Execution failed")
        if run.stderr:
            print(run.stderr)
        return []

    raw_path = os.path.join(raw_dir, f"{micro_arch_name(arch, core)}.txt")
    with open(raw_path, "w") as f:
        f.write(run.stdout)
    print(f"Saved raw output to {raw_path}")

    rows = parse_output(run.stdout)
    if not rows:
        print("No results to save or plot")
        return []

    tsv_path = tsv_path_for(arch, core)
    write_tsv(rows, tsv_path)
    print(f"Saved parsed results to {tsv_path}")
    return rows


def plot_bar_chart(rows, arch, core):
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

    width = min(max(args.probe_positions / 10, 10), 28)
    fig, ax = plt.subplots(1, 1, figsize=(width, 4), sharex=True)

    sorted_rows = sorted(rows, key=lambda row: row["position"])
    positions = [row["position"] for row in sorted_rows]
    values = [row["avg_ns"] for row in sorted_rows]
    colors = [ROLE_COLORS.get(row["role"], ROLE_COLORS["cache_miss"]) for row in sorted_rows]

    ax.bar(positions, values, color=colors, width=0.85, edgecolor="black", linewidth=0.25)
    ax.axhline(args.hit_threshold_ns, color="black", linestyle="--", linewidth=0.9)
    instr = "Load instruction (ldrb)" if args.access == "load" else "Store instruction (strb)"
    ax.set_title(instr, loc="left", pad=4)
    ax.set_ylabel("Average reload ns")
    ax.set_ylim(0, max(300, max(values) * 1.05 if values else 300))
    ax.grid(axis="y", alpha=0.25)
    ax.set_xlim(-1, max(positions) + 1 if positions else args.probe_positions)

    tick_step = max(1, args.probe_positions // 16)
    ax.set_xticks(range(0, args.probe_positions, tick_step))
    ax.set_xlabel("Probe cache-line index")

    legend_items = [
        Patch(facecolor=ROLE_COLORS["accessed"], edgecolor="black", label="accessed"),
        Patch(facecolor=ROLE_COLORS["prefetched"], edgecolor="black", label="prefetched"),
        Patch(facecolor=ROLE_COLORS["cache_miss"], edgecolor="black", label="cache_miss"),
    ]
    ax.legend(handles=legend_items, loc="upper right", frameon=False, ncol=3)

    fig.suptitle(
        f"{arch} core {core}, stride={args.stride} lines, "
        f"train_step={args.train_step}, access={args.access}, "
        f"hit_threshold={args.hit_threshold_ns} ns",
        x=0.01,
        ha="left",
    )
    fig.tight_layout(rect=(0, 0, 1, 0.95))
    plot_path = plot_path_for(arch, core)
    plt.savefig(plot_path, dpi=300)
    plt.close(fig)
    print(f"Saved bar chart to {plot_path}")


if __name__ == "__main__":
    ensure_dirs()
    targets = get_targets()
    for arch, core in targets:
        if args.plot_only:
            tsv_path = tsv_path_for(arch, core)
            if not os.path.exists(tsv_path):
                print(f"Error: TSV result '{tsv_path}' not found.")
                continue
            result_rows = read_tsv(tsv_path)
        else:
            result_rows = run_tests(arch, core)
        if result_rows:
            plot_bar_chart(result_rows, arch, core)
