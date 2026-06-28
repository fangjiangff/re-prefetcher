import argparse
import csv
import os
import subprocess
import sys

from cross_test_config import (
    apply_single_core_defaults,
    apply_threshold_defaults,
    arch_choices,
    is_x86_arch,
    timer_define_for_arch,
    timer_unit_for_arch,
)

os.environ.setdefault("MPLCONFIGDIR", "/tmp/matplotlib-cache")

BASE_DIR = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.join(BASE_DIR, "test0-degree.c")
UTIL_SRC = os.path.join(BASE_DIR, "until.c")
OUT = os.path.join(BASE_DIR, "bin", "test0-degree")
RESULT_DIR = os.path.join(BASE_DIR, "res", "store-stride")
RAW_DIR = os.path.join(RESULT_DIR, "raw")
GREEN = "\033[32m"
RESET = "\033[0m"

DEFAULT_STRIDE_LINES = 5
DEFAULT_MAX_STEP = 20
DEFAULT_PROBE_POSITIONS = 40
DEFAULT_ROUNDS = 100
PAGE_BOUNDARY_BYTES = 4096


def parse_args():
    parser = argparse.ArgumentParser(
        description="Sweep access count for a fixed stride and plot every probed position."
    )
    parser.add_argument("--arch", required=True, choices=arch_choices(),
                        help="Architecture label used to select default core.")
    parser.add_argument("--core", type=int, default=None,
                        help="Override CPU core used by taskset. Default is selected from --arch.")
    parser.add_argument("--stride", type=int, default=DEFAULT_STRIDE_LINES,
                        help=f"Fixed stride in cache lines. Default: {DEFAULT_STRIDE_LINES}")
    parser.add_argument("--max-step", type=int, default=DEFAULT_MAX_STEP,
                        help=f"Maximum access count. Default: {DEFAULT_MAX_STEP}")
    parser.add_argument("--probe-positions", type=int, default=DEFAULT_PROBE_POSITIONS,
                        help=f"Positions to probe. Default: {DEFAULT_PROBE_POSITIONS}")
    parser.add_argument("--rounds", type=int, default=DEFAULT_ROUNDS,
                        help=f"Rounds per access-count/probe-position point. Default: {DEFAULT_ROUNDS}")
    parser.add_argument("--access", choices=["store", "load", "prefetch"],
                        default="store",
                        help=("Access instruction used for training/trigger. "
                              "prefetch uses the architecture-specific "
                              "prefetch instruction. Default: store"))
    parser.add_argument("--hit-threshold-ns", dest="threshold_ns", type=int, default=180,
                        help="Candidate latency <= this value is treated as prefetched. "
                             "Default is selected from --arch.")
    parser.add_argument("--timer", choices=["gettime", "rdtsc"],
                        default="gettime",
                        help=("x86 timestamp source. gettime uses "
                              "clock_gettime; rdtsc uses rdtscp. "
                              "Default: gettime"))
    parser.add_argument("--cc", default=os.environ.get("CC", "gcc"),
                        help="Compiler command. Default: $CC or gcc")
    parser.add_argument("--output", default=None,
                        help="TSV output path. Default includes arch/core/access/stride.")
    parser.add_argument("--raw-output", default=None,
                        help="Raw program output path. Default includes arch/core/access/stride.")
    parser.add_argument("--plot-output", default=None,
                        help="Heatmap output path. Default includes arch/core/access/stride.")
    parser.add_argument("--plot-only", action="store_true",
                        help="Read --output and generate the heatmap without compiling/running.")
    parser.add_argument("--no-plot", action="store_true")
    parser.add_argument("--no-compile", action="store_true")
    args = parser.parse_args()

    apply_single_core_defaults(args)
    apply_threshold_defaults(args)

    if args.core < 0:
        parser.error("--core must be >= 0")
    if args.stride < 1:
        parser.error("--stride must be >= 1")
    if args.max_step < 1:
        parser.error("--max-step must be >= 1")
    if args.probe_positions < args.max_step:
        parser.error("--probe-positions must be >= --max-step")
    if args.rounds < 1:
        parser.error("--rounds must be >= 1")
    if args.threshold_ns < 1:
        parser.error("--hit-threshold-ns must be >= 1")
    apply_default_paths(args)
    return args


def result_name(args):
    timer_suffix = ""
    if is_x86_arch(args.arch) and args.timer != "gettime":
        timer_suffix = f"-timer{args.timer}"
    return (
        f"{args.arch}-core{args.core}-degree-sweep"
        f"-{args.access}-stride{args.stride}"
        f"-maxstep{args.max_step}-probe{args.probe_positions}"
        f"{timer_suffix}"
    )


def apply_default_paths(args):
    name = result_name(args)
    if args.output is None:
        args.output = os.path.join(RESULT_DIR, f"{name}.tsv")
    if args.raw_output is None:
        args.raw_output = os.path.join(RAW_DIR, f"{name}.txt")
    if args.plot_output is None:
        args.plot_output = os.path.join(RESULT_DIR, f"{name}-heatmap.png")


def ensure_parent(path):
    parent = os.path.dirname(path)
    if parent:
        os.makedirs(parent, exist_ok=True)


def compile_test(args):
    os.makedirs(os.path.dirname(OUT), exist_ok=True)
    compile_cmd = [
        args.cc,
        "-std=gnu11",
        "-O0",
        "-static",
        f"-DTEST_STRIDE={args.stride * 64}",
        f"-DMAX_STEP={args.max_step}",
        f"-DPROBES={args.probe_positions}",
        f"-DROUNDS={args.rounds}",
        f"-DTRAIN_ACCESS_LOAD={1 if args.access == 'load' else 0}",
        f"-DTRAIN_ACCESS_PREFETCH={1 if args.access == 'prefetch' else 0}",
        "-o",
        OUT,
        SRC,
        UTIL_SRC,
    ]
    timer_define = timer_define_for_arch(args.arch, args.timer)
    if timer_define is not None:
        compile_cmd.insert(-4, timer_define)
    return subprocess.run(compile_cmd)


def run_test(args):
    return subprocess.run(
        ["taskset", "-c", str(args.core), OUT],
        capture_output=True,
        text=True,
    )


def parse_rows(text):
    rows = []
    for line in text.splitlines():
        stripped = line.strip()
        if not stripped or stripped.startswith("#"):
            continue

        parts = stripped.split()
        if len(parts) != 5:
            raise ValueError(f"unexpected output row: {line}")

        train_step = int(parts[0])
        probe_pos = int(parts[1])
        offset_bytes = int(parts[2])
        role = parts[3]
        avg_ns = int(parts[4])
        rows.append({
            "train_step": train_step,
            "probe_pos": probe_pos,
            "offset_bytes": offset_bytes,
            "role": role,
            "avg_ns": avg_ns,
        })

    if not rows:
        raise ValueError("no plottable result data found")
    return rows


def write_tsv(path, rows):
    ensure_parent(path)
    with open(path, "w", newline="") as f:
        writer = csv.DictWriter(
            f,
            fieldnames=["train_step", "probe_pos", "offset_bytes", "role", "avg_ns"],
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
                "train_step": int(row["train_step"]),
                "probe_pos": int(row["probe_pos"]),
                "offset_bytes": int(row["offset_bytes"]),
                "role": row["role"],
                "avg_ns": int(row["avg_ns"]),
            })
    if not rows:
        raise ValueError("no plottable TSV rows found")
    return rows


def infer_stride_bytes(rows):
    for row in rows:
        probe_pos = row["probe_pos"]
        if probe_pos > 0 and row["offset_bytes"] % probe_pos == 0:
            return row["offset_bytes"] // probe_pos
    return None


def draw_page_boundaries(ax, rows, probe_labels):
    stride_bytes = infer_stride_bytes(rows)
    if stride_bytes is None:
        return

    max_extent = (max(probe_labels) + 1) * stride_bytes
    drawn_positions = set()
    for boundary in range(PAGE_BOUNDARY_BYTES, max_extent, PAGE_BOUNDARY_BYTES):
        x = (boundary + stride_bytes - 1) // stride_bytes
        if x in drawn_positions:
            continue
        if 0 < x < len(probe_labels):
            drawn_positions.add(x)
            ax.axvline(
                x=x,
                color="red",
                linewidth=2.0,
                alpha=0.95,
                zorder=20,
            )


def plot_heatmap(path, rows, title, unit):
    try:
        import matplotlib.pyplot as plt
        import numpy as np
        import seaborn as sns
    except ModuleNotFoundError as exc:
        print(f"Skipping heatmap: missing Python package '{exc.name}'.")
        print("Install plotting dependencies with:")
        print("  sudo apt install python3-matplotlib python3-seaborn")
        print("or:")
        print("  python3 -m pip install matplotlib seaborn")
        return

    ensure_parent(path)
    step_labels = sorted({row["train_step"] for row in rows})
    probe_labels = sorted({row["probe_pos"] for row in rows})
    step_index = {value: i for i, value in enumerate(step_labels)}
    probe_index = {value: i for i, value in enumerate(probe_labels)}

    matrix = np.full((len(step_labels), len(probe_labels)), np.nan)
    accessed_mask = np.zeros((len(step_labels), len(probe_labels)), dtype=bool)
    for row in rows:
        row_idx = step_index[row["train_step"]]
        col_idx = probe_index[row["probe_pos"]]
        matrix[row_idx, col_idx] = row["avg_ns"]
        if row["role"] == "accessed":
            accessed_mask[row_idx, col_idx] = True

    masked = np.ma.masked_where(accessed_mask | np.isnan(matrix), matrix)
    accessed_overlay = np.where(accessed_mask, 1.0, np.nan)
    width = max(9.0, 0.22 * len(probe_labels) + 3.0)
    height = max(6.0, 0.28 * len(step_labels) + 2.5)
    fig, ax = plt.subplots(figsize=(width, height))
    sns.heatmap(
        masked,
        cmap="RdYlBu_r",
        annot=False,
        ax=ax,
        vmin=20,
        vmax=300,
        cbar=True,
        cbar_kws={"pad": 0.01},
        linewidths=0.35,
        linecolor=(1.0, 1.0, 1.0, 0.45),
        yticklabels=False,
    )
    sns.heatmap(
        accessed_overlay,
        cmap=sns.color_palette(["#BDBDBD"], as_cmap=True),
        annot=False,
        ax=ax,
        cbar=False,
        linewidths=0.35,
        linecolor=(1.0, 1.0, 1.0, 0.45),
        yticklabels=False,
    )

    ax.set_title(title, loc="left", pad=8, fontsize=16)
    ax.set_xlabel("Probe position", fontsize=14)
    ax.set_ylabel("Access count", fontsize=14)

    x_tick_step = max(1, len(probe_labels) // 24)
    x_ticks = list(range(0, len(probe_labels), x_tick_step))
    ax.set_xticks(np.array(x_ticks) + 0.5)
    ax.set_xticklabels([probe_labels[i] + 1 for i in x_ticks], fontsize=12)

    ax.set_yticks(np.arange(len(step_labels)) + 0.5)
    ax.set_yticklabels(step_labels, fontsize=12)

    draw_page_boundaries(ax, rows, probe_labels)

    cbar = ax.collections[0].colorbar
    if cbar is not None:
        cbar.set_label(f"Average reload latency ({unit})", fontsize=14)
        cbar.ax.tick_params(labelsize=12)
    fig.tight_layout()
    fig.savefig(path, dpi=300)
    plt.close(fig)


def print_summary(rows, threshold_ns, unit):
    degree_by_step = {}
    for row in rows:
        if row["role"] != "candidate":
            continue
        if row["train_step"] not in degree_by_step:
            degree_by_step[row["train_step"]] = 0
        if row["avg_ns"] <= threshold_ns:
            degree_by_step[row["train_step"]] += 1

    print(f"{GREEN}# degree summary")
    print(f"# prefetch threshold: candidate avg_{unit} <= {threshold_ns}")
    positive_degrees = [
        degree for degree in degree_by_step.values()
        if degree > 0
    ]

    if not positive_degrees:
        print("min_prefetch_degree\tN/A")
        print("max_prefetch_degree\tN/A")
        print(RESET, end="")
        return

    min_degree = min(positive_degrees)
    max_degree = max(positive_degrees)

    print(f"min_prefetch_degree\t{min_degree}")
    print(f"max_prefetch_degree\t{max_degree}")
    print(RESET, end="")


def main():
    args = parse_args()
    unit = timer_unit_for_arch(args.arch, args.timer)

    if args.plot_only:
        rows = read_tsv(args.output)
    else:
        if not args.no_compile:
            compiled = compile_test(args)
            if compiled.returncode != 0:
                print("Compile failed", file=sys.stderr)
                return compiled.returncode

        run = run_test(args)
        if run.stdout:
            print(run.stdout, end="")
        if run.stderr:
            print(run.stderr, end="", file=sys.stderr)
        if run.returncode != 0:
            print("Execution failed", file=sys.stderr)
            return run.returncode

        ensure_parent(args.raw_output)
        with open(args.raw_output, "w") as f:
            f.write(run.stdout)
        rows = parse_rows(run.stdout)
        write_tsv(args.output, rows)
        print(f"Saved TSV to {args.output}")
        print(f"Saved raw output to {args.raw_output}")

    print_summary(rows, args.threshold_ns, unit)

    if not args.no_plot:
        plot_heatmap(
            args.plot_output,
            rows,
            (
                f"{args.arch} core {args.core} {args.access} degree sweep "
                f"(stride={args.stride} lines)"
            ),
            unit,
        )
        print(f"Saved heatmap to {args.plot_output}")

    return 0


if __name__ == "__main__":
    sys.exit(main())
