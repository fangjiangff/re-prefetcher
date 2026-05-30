import argparse
import csv
import os
import subprocess
import sys


SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
SOURCE = os.path.join(SCRIPT_DIR, "test-speed.cc")
BIN_DIR = os.path.join(SCRIPT_DIR, "bin")
BIN = os.path.join(BIN_DIR, "test-speed")
DEFAULT_OUTPUT_DIR = os.path.join(SCRIPT_DIR, "res", "test-speed")


def parse_args():
    parser = argparse.ArgumentParser(
        description="Run test-speed.cc once and plot the per-sample cycle results."
    )
    parser.add_argument("--array-mb", type=int, default=256)
    parser.add_argument("--samples", type=int, default=1000)
    parser.add_argument("--seed", type=lambda x: int(x, 0), default=0x5eed1234)
    parser.add_argument("--output-dir", default=DEFAULT_OUTPUT_DIR)
    parser.add_argument("--cxx", default="g++")
    parser.add_argument("--prfm-mode", default="PLDL3KEEP")
    parser.add_argument(
        "--baseline-subtracted",
        action="store_true",
        help="Plot empty-baseline-subtracted cycles instead of raw measured cycles.",
    )
    parser.add_argument("--no-build", action="store_true")
    parser.add_argument("--plot-only", action="store_true")
    parser.add_argument("--csv", default=None)
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
    cmd = [BIN, str(args.array_mb), str(args.samples), hex(args.seed), csv_path]
    print("Running:", " ".join(cmd))
    output = subprocess.check_output(cmd, cwd=SCRIPT_DIR, text=True)
    print(output, end="")


def read_csv(path):
    rows = []
    with open(path, newline="") as f:
        reader = csv.DictReader(f)
        for row in reader:
            parsed = {}
            for key, value in row.items():
                parsed[key] = int(value)
            rows.append(parsed)
    return rows


def percentile(values, pct):
    if not values:
        return 0.0
    ordered = sorted(values)
    pos = (len(ordered) - 1) * pct / 100.0
    lo = int(pos)
    hi = min(lo + 1, len(ordered) - 1)
    frac = pos - lo
    return ordered[lo] * (1.0 - frac) + ordered[hi] * frac


def write_summary(rows, path):
    series = {
        "software_prefetch_raw": [row["prefetch_raw_cycles"] for row in rows],
        "miss_load_raw": [row["load_miss_raw_cycles"] for row in rows],
        "software_prefetch_subtracted": [row["prefetch_cycles"] for row in rows],
        "miss_load_subtracted": [row["load_miss_cycles"] for row in rows],
    }

    with open(path, "w", newline="") as f:
        writer = csv.writer(f)
        writer.writerow(["type", "mean", "median", "p05", "p95", "min", "max"])
        for name, values in series.items():
            mean = sum(values) / len(values)
            writer.writerow([
                name,
                f"{mean:.3f}",
                f"{percentile(values, 50):.3f}",
                f"{percentile(values, 5):.3f}",
                f"{percentile(values, 95):.3f}",
                min(values),
                max(values),
            ])


def plot(rows, output_dir, baseline_subtracted):
    os.makedirs(output_dir, exist_ok=True)
    os.environ.setdefault("MPLCONFIGDIR", os.path.join(output_dir, ".matplotlib"))

    try:
        import matplotlib.pyplot as plt
    except ImportError:
        print("matplotlib is not installed; CSV was generated, but figures were skipped.")
        return

    sample = [row["sample"] for row in rows]
    if baseline_subtracted:
        prefetch = [row["prefetch_cycles"] for row in rows]
        load_miss = [row["load_miss_cycles"] for row in rows]
        ylabel = "Cycles after empty-baseline subtraction"
        output_name = "test-speed-sw-missload-line-subtracted.png"
    else:
        prefetch = [row["prefetch_raw_cycles"] for row in rows]
        load_miss = [row["load_miss_raw_cycles"] for row in rows]
        ylabel = "Raw measured cycles"
        output_name = "test-speed-sw-missload-line.png"

    fig, ax = plt.subplots(figsize=(8.2, 4.2))
    ax.plot(sample, prefetch, linewidth=1.0, label="Software prefetch")
    ax.plot(sample, load_miss, linewidth=1.0, label="Load")
    ax.set_xlim(0, len(sample))
    ax.margins(x=0)
    ax.set_xlabel("Sample index")
    ax.set_ylabel(ylabel)
    ax.set_title("Per-sample cycles over 1000 trials")
    ax.grid(True, linestyle="--", linewidth=0.5, alpha=0.5)
    ax.legend()
    fig.tight_layout()
    fig.savefig(os.path.join(output_dir, output_name), dpi=300)
    plt.close(fig)


def main():
    args = parse_args()
    args.output_dir = os.path.abspath(args.output_dir)
    os.makedirs(args.output_dir, exist_ok=True)
    csv_path = os.path.abspath(args.csv) if args.csv else os.path.join(args.output_dir, "test-speed-raw.csv")

    if not args.plot_only:
        if not args.no_build:
            build_binary(args)
        run_test(args, csv_path)

    rows = read_csv(csv_path)
    summary_path = os.path.join(args.output_dir, "test-speed-summary.csv")
    write_summary(rows, summary_path)
    plot(rows, args.output_dir, args.baseline_subtracted)

    print(f"Raw CSV written to {csv_path}")
    print(f"Summary CSV written to {summary_path}")
    print(f"Figures written under {args.output_dir}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
