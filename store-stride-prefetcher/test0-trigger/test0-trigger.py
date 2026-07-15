import argparse
import csv
import os
import subprocess
import sys

ROOT_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
if ROOT_DIR not in sys.path:
    sys.path.insert(0, ROOT_DIR)

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
SRC = os.path.join(BASE_DIR, "test0-trigger.c")
UTIL_SRC = os.path.join(ROOT_DIR, "until.c")
OUT = os.path.join(ROOT_DIR, "bin", "test0-trigger")
RESULT_DIR = os.path.join(ROOT_DIR, "res", "store-stride")
RAW_DIR = os.path.join(RESULT_DIR, "raw")
GREEN = "\033[32m"
RESET = "\033[0m"


def parse_args():
    parser = argparse.ArgumentParser(
        description="Sweep store-stride trigger conditions and plot the first candidate prefetch latency."
    )
    parser.add_argument("--arch", required=True, choices=arch_choices(),
                        help="Architecture label used to select default core.")
    parser.add_argument("--core", type=int, default=None,
                        help="Override CPU core used by taskset. Default is selected from --arch.")
    parser.add_argument("--rounds", type=int, default=4000,
                        help="Rounds per stride/access-count point. Default: 100")
    parser.add_argument("--max-stride", type=int, default=2048,
                        help="Maximum stride in bytes. Default: 2048")
    parser.add_argument("--max-step", type=int, default=40,
                        help="Maximum access count. Default: 10")
    parser.add_argument("--access", choices=["store", "load", "prefetch"],
                        default="store",
                        help=("Access instruction used for training/trigger. "
                              "prefetch uses the architecture-specific "
                              "prefetch instruction. Default: store"))
    parser.add_argument("--hit-threshold-ns", dest="threshold_ns", type=int, default=None,
                        help="Latency <= this value is treated as prefetched. "
                             "Default is selected from --arch.")
    parser.add_argument("--timer", choices=["gettime", "rdtsc"],
                        default="gettime",
                        help=("x86 timestamp source. gettime uses "
                              "clock_gettime; rdtsc uses rdtscp. "
                              "Default: gettime"))
    parser.add_argument("--cc", default=os.environ.get("CC", "gcc"),
                        help="Compiler command. Default: $CC or gcc")
    parser.add_argument("--output", default=None,
                        help="TSV output path. Default includes arch/core/access.")
    parser.add_argument("--raw-output", default=None,
                        help="Raw program output path. Default includes arch/core/access.")
    parser.add_argument("--plot-output", default=None,
                        help="Heatmap output path. Default includes arch/core/access.")
    parser.add_argument("--plot-only", action="store_true",
                        help="Read --output and generate the heatmap without compiling/running.")
    parser.add_argument("--no-plot", action="store_true")
    parser.add_argument("--no-compile", action="store_true")
    args = parser.parse_args()

    apply_single_core_defaults(args)
    apply_threshold_defaults(args)

    if args.core < 0:
        parser.error("--core must be >= 0")
    if args.rounds < 1:
        parser.error("--rounds must be >= 1")
    if args.max_stride < 64 or args.max_stride % 64 != 0:
        parser.error("--max-stride must be a positive multiple of 64")
    if args.max_step < 1:
        parser.error("--max-step must be >= 1")
    if args.threshold_ns < 1:
        parser.error("--hit-threshold-ns must be >= 1")
    apply_default_paths(args)
    return args


def result_name(args):
    timer_suffix = ""
    if is_x86_arch(args.arch) and args.timer != "gettime":
        timer_suffix = f"-timer{args.timer}"
    return (
        f"{args.arch}-core{args.core}-trigger-sweep"
        f"-{args.access}-maxstride{args.max_stride}-maxstep{args.max_step}"
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


def arch_cflags_for(arch):
    if is_x86_arch(arch):
        return []
    return ["-march=armv8.5-a+predres"]


def compile_test(args):
    os.makedirs(os.path.dirname(OUT), exist_ok=True)
    compile_cmd = [
        args.cc,
        "-std=gnu11",
        "-O0",
        "-static",
        f"-DROUNDS={args.rounds}",
        f"-DMAX_STRIDE={args.max_stride}",
        f"-DMAX_STEP={args.max_step}",
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
    compile_cmd[1:1] = arch_cflags_for(args.arch)
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
        if len(parts) != 3:
            raise ValueError(f"unexpected output row: {line}")

        stride_lines, train_step, avg_ns = (int(value) for value in parts)
        rows.append({
            "stride_lines": stride_lines,
            "stride_bytes": stride_lines * 64,
            "train_step": train_step,
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
            fieldnames=["stride_lines", "stride_bytes", "train_step", "avg_ns"],
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
                "stride_lines": int(row["stride_lines"]),
                "stride_bytes": int(row["stride_bytes"]),
                "train_step": int(row["train_step"]),
                "avg_ns": int(row["avg_ns"]),
            })
    if not rows:
        raise ValueError("no plottable TSV rows found")
    return rows


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
    stride_labels = sorted({row["stride_lines"] for row in rows})
    step_labels = sorted({row["train_step"] for row in rows})
    stride_index = {value: i for i, value in enumerate(stride_labels)}
    step_index = {value: i for i, value in enumerate(step_labels)}

    matrix = np.full((len(stride_labels), len(step_labels)), np.nan)
    for row in rows:
        matrix[stride_index[row["stride_lines"]], step_index[row["train_step"]]] = row["avg_ns"]

    masked = np.ma.masked_invalid(matrix)

    width = max(8.0, 0.45 * len(step_labels) + 3.0)
    height = max(7.0, 0.18 * len(stride_labels) + 2.5)
    fig, ax = plt.subplots(figsize=(width, height))
    sns.heatmap(
        masked,
        cmap="RdYlBu_r",
        annot=False,
        ax=ax,
        # center=180,
        vmin=0,
        vmax=150,
        cbar=True,
        cbar_kws={"pad": 0.01},
        yticklabels=False,
    )

    ax.set_title(title, loc="left", pad=8, fontsize=16)
    ax.set_xlabel("Access count", fontsize=14)
    ax.set_ylabel("Stride (cache lines)", fontsize=14)
    ax.set_xticks(np.arange(len(step_labels)) + 0.5)
    ax.set_xticklabels(step_labels, fontsize=12)

    y_tick_step = max(1, len(stride_labels) // 24)
    y_ticks = list(range(0, len(stride_labels), y_tick_step))
    ax.set_yticks(np.array(y_ticks) + 0.5)
    ax.set_yticklabels([stride_labels[i] for i in y_ticks], fontsize=12)

    cbar = ax.collections[0].colorbar
    if cbar is not None:
        cbar.set_label(f"Average reload latency ({unit})", fontsize=14)
        cbar.ax.tick_params(labelsize=12)
    fig.tight_layout()
    fig.savefig(path, dpi=300)
    plt.close(fig)


def print_summary(rows, threshold_ns, unit):
    prefetched = [row for row in rows if row["avg_ns"] <= threshold_ns]

    print(f"{GREEN}# trigger summary")
    print(f"# prefetch threshold: avg_{unit} <= {threshold_ns}")
    if not prefetched:
        print("min_access_times\tN/A")
        print("min_stride_lines\tN/A")
        print("max_stride_lines\tN/A")
        print(RESET, end="")
        return

    min_access_times = min(row["train_step"] for row in prefetched)
    min_stride_lines = min(row["stride_lines"] for row in prefetched)
    max_stride_lines = max(row["stride_lines"] for row in prefetched)

    print(f"min_access_times\t{min_access_times}")
    print(f"min_stride_lines\t{min_stride_lines}")
    print(f"max_stride_lines\t{max_stride_lines}")
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
            f"{args.arch} core {args.core} {args.access} stride trigger sweep",
            unit,
        )
        print(f"Saved heatmap to {args.plot_output}")

    return 0


if __name__ == "__main__":
    sys.exit(main())
