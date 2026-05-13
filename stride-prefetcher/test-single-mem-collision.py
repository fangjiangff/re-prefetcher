import argparse
import os
import subprocess
import sys

os.environ.setdefault("MPLCONFIGDIR", "/tmp/matplotlib-cache")
import matplotlib.pyplot as plt
import numpy as np


SRC = "test-single-mem-collision.c"
OUT = "bin/test-single-mem-collision"
DEFAULT_PREFETCH_PC = "0x500000120"
DEFAULT_VICTIM_BUFFER = "0x600000000"
DEFAULT_MIN_DIFF_BIT = 0
DEFAULT_MAX_DIFF_BIT = 47
DEFAULT_ROUNDS = 1000
DEFAULT_CORE = 2
DEFAULT_RESULT = "res/mem-collision.tsv"
DEFAULT_PLOT = "res/mem-collision.png"


def compile_test():
    os.makedirs(os.path.dirname(OUT), exist_ok=True)
    compile_cmd = [
        "gcc",
        "-std=gnu11",
        "-O0",
        "-static",
        "-o",
        OUT,
        SRC,
    ]
    return subprocess.run(compile_cmd)


def run_test(args):
    run_cmd = [
        "taskset",
        "-c",
        str(args.core),
        "./" + OUT,
        args.prefetch_pc,
        args.victim_buffer,
        str(args.min_diff_bit),
        str(args.max_diff_bit),
        str(args.rounds),
    ]
    return subprocess.run(run_cmd, capture_output=True, text=True)


def save_result(path, output):
    if not path:
        return
    result_dir = os.path.dirname(path)
    if result_dir:
        os.makedirs(result_dir, exist_ok=True)
    with open(path, "w") as f:
        f.write(output)


def parse_result(output):
    columns = []
    row_labels = []
    matrix = []

    for line in output.splitlines():
        stripped = line.strip()
        if not stripped or stripped.startswith("#"):
            continue

        parts = stripped.split()
        if not parts:
            continue

        if parts[0] == "diff_bit":
            columns = parts[1:]
            continue

        if not columns:
            continue

        if len(parts) != len(columns) + 1:
            raise ValueError(f"unexpected result row: {line}")

        row_labels.append(parts[0])
        matrix.append([float(value) for value in parts[1:]])

    if not columns or not matrix:
        raise ValueError("no plottable result data found")

    return columns, row_labels, np.array(matrix, dtype=float)


def plot_result(path, output, title, vmin=None, vmax=None):
    if not path:
        return

    columns, row_labels, matrix = parse_result(output)
    plot_dir = os.path.dirname(path)
    if plot_dir:
        os.makedirs(plot_dir, exist_ok=True)

    masked = np.ma.masked_less(matrix, 0)
    if vmin is None:
        vmin = float(masked.min())
    if vmax is None:
        vmax = float(masked.max())

    height = max(5.0, 0.22 * len(row_labels) + 2.2)
    fig, ax = plt.subplots(figsize=(10.5, height))
    cmap = plt.get_cmap("RdYlBu_r").copy()
    cmap.set_bad("#E6E6E6")

    im = ax.imshow(masked, aspect="auto", cmap=cmap, vmin=vmin, vmax=vmax)
    ax.set_title(title, loc="left", pad=8)
    ax.set_xlabel("train step")
    ax.set_ylabel("memory diff bit")

    ax.set_xticks(range(len(columns)))
    ax.set_xticklabels(columns, rotation=45, ha="right")
    ax.set_yticks(range(len(row_labels)))
    ax.set_yticklabels(row_labels)

    ax.grid(which="minor", color="white", linewidth=0.6)
    ax.set_xticks(np.arange(-0.5, len(columns), 1), minor=True)
    ax.set_yticks(np.arange(-0.5, len(row_labels), 1), minor=True)
    ax.tick_params(which="minor", bottom=False, left=False)

    cbar = fig.colorbar(im, ax=ax, pad=0.015)
    cbar.set_label("latency (ns)")

    fig.tight_layout()
    fig.savefig(path, dpi=300)
    plt.close(fig)


def main():
    parser = argparse.ArgumentParser(
        description="Compile and run test-single-mem-collision.c."
    )
    parser.add_argument("--prefetch-pc", default=DEFAULT_PREFETCH_PC)
    parser.add_argument("--victim-buffer", default=DEFAULT_VICTIM_BUFFER)
    parser.add_argument("--min-diff-bit", type=int, default=DEFAULT_MIN_DIFF_BIT)
    parser.add_argument("--max-diff-bit", type=int, default=DEFAULT_MAX_DIFF_BIT)
    parser.add_argument("--rounds", type=int, default=DEFAULT_ROUNDS)
    parser.add_argument("--core", type=int, default=DEFAULT_CORE)
    parser.add_argument("--output", default=DEFAULT_RESULT)
    parser.add_argument("--plot-output", default=DEFAULT_PLOT)
    parser.add_argument("--plot-vmin", type=float, default=None)
    parser.add_argument("--plot-vmax", type=float, default=None)
    parser.add_argument("--no-plot", action="store_true")
    parser.add_argument("--no-compile", action="store_true")
    args = parser.parse_args()

    if not args.no_compile:
        res = compile_test()
        if res.returncode != 0:
            print("Compile failed", file=sys.stderr)
            return res.returncode

    run = run_test(args)
    if run.stdout:
        print(run.stdout, end="")
    if run.stderr:
        print(run.stderr, end="", file=sys.stderr)
    if run.returncode != 0:
        print("Execution failed", file=sys.stderr)
        return run.returncode

    save_result(args.output, run.stdout)
    if args.output:
        print(f"Saved result to {args.output}")

    if not args.no_plot:
        plot_title = (
            f"Single memory collision latency "
            f"(victim_buffer={args.victim_buffer}, rounds={args.rounds})"
        )
        try:
            plot_result(
                args.plot_output,
                run.stdout,
                plot_title,
                vmin=args.plot_vmin,
                vmax=args.plot_vmax,
            )
        except ValueError as exc:
            print(f"Plot failed: {exc}", file=sys.stderr)
            return 1
        if args.plot_output:
            print(f"Saved plot to {args.plot_output}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
