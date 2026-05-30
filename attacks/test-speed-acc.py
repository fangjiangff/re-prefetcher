import argparse
import csv
import os
import subprocess
import sys


SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
SOURCE = os.path.join(SCRIPT_DIR, "test-speed-acc.cc")
BIN_DIR = os.path.join(SCRIPT_DIR, "bin")
BIN = os.path.join(BIN_DIR, "test-speed-acc")
DEFAULT_CSV = os.path.join(SCRIPT_DIR, "res", "test-speed-acc.csv")
DEFAULT_OUTPUT = os.path.join(SCRIPT_DIR, "res", "test-speed-acc-line.png")


def parse_args():
    parser = argparse.ArgumentParser(description="Plot test-speed-acc grouped miss-access results.")
    parser.add_argument("--array-mb", type=int, default=256)
    parser.add_argument("--trials", type=int, default=1000)
    parser.add_argument("--seed", type=lambda x: int(x, 0), default=0x5eed1234)
    parser.add_argument("--csv", default=DEFAULT_CSV)
    parser.add_argument("--output", default=DEFAULT_OUTPUT)
    parser.add_argument("--cxx", default="g++")
    parser.add_argument("--prfm-mode", default="PLDL1KEEP")
    parser.add_argument("--no-build", action="store_true")
    parser.add_argument("--plot-only", action="store_true")
    parser.add_argument(
        "--per-element",
        action="store_true",
        help="Plot cycles per element instead of total cycles per group.",
    )
    return parser.parse_args()


def build_binary(args):
    os.makedirs(BIN_DIR, exist_ok=True)
    cmd = [
        args.cxx,
        "-std=gnu++11",
        "-O2",
        "-Wall",
        "-Wextra",
        f"-DPRFM_MODE={args.prfm_mode}",
        "-o",
        BIN,
        SOURCE,
    ]
    print("Building:", " ".join(cmd))
    subprocess.check_call(cmd, cwd=SCRIPT_DIR)


def run_test(args, csv_path):
    os.makedirs(os.path.dirname(csv_path), exist_ok=True)
    cmd = [BIN, str(args.array_mb), str(args.trials), hex(args.seed), csv_path]
    print("Running:", " ".join(cmd))
    output = subprocess.check_output(cmd, cwd=SCRIPT_DIR, text=True)
    print(output, end="")


def read_rows(path):
    rows = []
    with open(path, newline="") as f:
        reader = csv.DictReader(f)
        for row in reader:
            rows.append(
                {
                    "count": int(row["count"]),
                    "prefetch_avg_cycles": float(row["prefetch_avg_cycles"]),
                    "load_avg_cycles": float(row["load_avg_cycles"]),
                    "prefetch_cycles_per_elem": float(row["prefetch_cycles_per_elem"]),
                    "load_cycles_per_elem": float(row["load_cycles_per_elem"]),
                }
            )
    return rows


def plot(rows, output, per_element):
    output = os.path.abspath(output)
    os.makedirs(os.path.dirname(output), exist_ok=True)
    os.environ.setdefault("MPLCONFIGDIR", os.path.join(os.path.dirname(output), ".matplotlib"))

    try:
        import matplotlib.pyplot as plt
    except ImportError:
        print("matplotlib is not installed; cannot generate the line chart.", file=sys.stderr)
        return 1

    counts = [row["count"] for row in rows]
    if per_element:
        prefetch = [row["prefetch_cycles_per_elem"] for row in rows]
        load = [row["load_cycles_per_elem"] for row in rows]
        ylabel = "Cycles per missed cache line"
        title = "Per-element cost for grouped cache misses"
    else:
        prefetch = [row["prefetch_avg_cycles"] for row in rows]
        load = [row["load_avg_cycles"] for row in rows]
        ylabel = "Cycles per group"
        title = "Grouped cache-miss access cost"

    fig, ax = plt.subplots(figsize=(7.2, 4.2))
    ax.plot(counts, prefetch, marker="o", linewidth=1.8, label="Software prefetch")
    ax.plot(counts, load, marker="s", linewidth=1.8, label="Load")
    ax.set_xscale("log", base=2)
    ax.set_xticks(counts)
    ax.set_xticklabels([str(value) for value in counts])
    ax.set_xlabel("Number of cache-miss elements")
    ax.set_ylabel(ylabel)
    ax.set_title(title)
    ax.grid(True, linestyle="--", linewidth=0.6, alpha=0.5)
    ax.legend()
    fig.tight_layout()
    fig.savefig(output, dpi=300)
    plt.close(fig)
    print(f"Saved line chart to {output}")
    return 0


def main():
    args = parse_args()
    csv_path = os.path.abspath(args.csv)
    output_path = os.path.abspath(args.output)

    if not args.plot_only:
        if not args.no_build:
            build_binary(args)
        run_test(args, csv_path)

    rows = read_rows(csv_path)
    return plot(rows, output_path, args.per_element)


if __name__ == "__main__":
    sys.exit(main())
