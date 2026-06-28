import argparse
import csv
import os
import platform
import re
import subprocess
import sys


SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
SOURCE = os.path.join(SCRIPT_DIR, "test-load-vs-sw-latency.cc")
BIN_DIR = os.path.join(SCRIPT_DIR, "bin")
BIN = os.path.join(BIN_DIR, "test-load-vs-sw-latency")
DEFAULT_OUTPUT_DIR = os.path.join(SCRIPT_DIR, "res", "test-load-vs-sw-latency")

# ARCHES = ["RaptorCove", "Gracemont"]
# ARCHES = ["RC", "GM","CL"]
# CORES = [0, 16, 0]
ARCHES = ["A76","A78", "A55", "A725", "X925","CL","RC","GM","Zen4"]
CORES = [2, 4, 1, 4, 6, 0, 0, 16,0]


def parse_args():
    parser = argparse.ArgumentParser(
        description="Run test-load-vs-sw-latency.cc for every software prefetch type."
    )
    parser.add_argument("--array-mb", type=int, default=256)
    parser.add_argument("--samples", type=int, default=1000)
    parser.add_argument("--seed", type=lambda x: int(x, 0), default=0x5eed1234)
    parser.add_argument("--output-dir", default=DEFAULT_OUTPUT_DIR)
    parser.add_argument("--cxx", default="g++")
    parser.add_argument("-a", "--arch", type=str.upper, choices=ARCHES, default="A76")
    parser.add_argument("-c", "--core", type=int, default=None,
                        help="Override the core selected by --arch for output naming.")
    parser.add_argument(
        "--use-cntvct",
        action="store_true",
        help="Use CNTVCT_EL0 on AArch64. This is the default.",
    )
    parser.add_argument(
        "--use-pmccntr",
        dest="use_cntvct",
        action="store_false",
        help="Use PMCCNTR_EL0 on AArch64. This may SIGILL unless EL0 PMU access is enabled.",
    )
    parser.add_argument(
        "--use-clock-gettime",
        action="store_true",
        help="Use clock_gettime(CLOCK_MONOTONIC_RAW) instead of a hardware cycle counter.",
    )
    parser.add_argument(
        "--baseline-subtracted",
        action="store_true",
        help="Plot empty-baseline-subtracted cycles instead of raw measured cycles.",
    )
    parser.add_argument("--no-build", action="store_true")
    parser.add_argument("--plot-only", action="store_true")
    parser.add_argument("--csv", default=None)
    parser.set_defaults(use_cntvct=True)
    return parser.parse_args()


def sanitize_filename_part(value):
    value = value.strip().lower()
    value = re.sub(r"[^a-z0-9._-]+", "-", value)
    value = re.sub(r"-+", "-", value).strip("-")
    return value or "unknown"


def core_for_arch(arch):
    arch = arch.upper()
    for idx, name in enumerate(ARCHES):
        if name.upper() == arch:
            return CORES[idx]
    supported = ", ".join(ARCHES)
    raise SystemExit(f"unsupported arch: {arch}; supported arches: {supported}")


def make_filename_tag(args):
    machine = sanitize_filename_part(platform.machine() or "unknown")
    arch = sanitize_filename_part(args.arch)
    return f"{machine}-{arch}-core{args.core}"


def build_binary(args):
    os.makedirs(BIN_DIR, exist_ok=True)
    cmd = [
        args.cxx,
        "-std=gnu++11",
        "-O0",
        "-Wall",
        "-Wextra",
        f"-DUSE_CNTVCT={1 if args.use_cntvct else 0}",
        f"-DUSE_CLOCK_GETTIME={1 if args.use_clock_gettime else 0}",
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
                if key in ("kind", "name"):
                    parsed[key] = value
                elif key == "samples":
                    parsed[key] = int(value)
                else:
                    parsed[key] = float(value)
            rows.append(parsed)
    return rows


def plot(rows, output_dir, baseline_subtracted, filename_tag):
    os.makedirs(output_dir, exist_ok=True)
    os.environ.setdefault("MPLCONFIGDIR", os.path.join(output_dir, ".matplotlib"))

    try:
        import matplotlib.pyplot as plt
    except ImportError:
        print("matplotlib is not installed; CSV was generated, but figures were skipped.")
        return

    if not rows:
        print("No rows found; figures were skipped.")
        return

    if baseline_subtracted:
        metric = "avg_cycles"
        ylabel = "Average cycles after empty-baseline subtraction"
        output_name = f"test-load-vs-sw-latency-{filename_tag}-avg-subtracted.png"
    else:
        metric = "raw_avg_cycles"
        ylabel = "Average raw measured cycles"
        output_name = f"test-load-vs-sw-latency-{filename_tag}-avg.png"

    labels = [row["name"] for row in rows]
    values = [row[metric] for row in rows]
    colors = ["#555555" if row["kind"] == "LOAD_MISS" else "#4c78a8" for row in rows]
    positions = list(range(len(rows)))

    fig_width = max(8.2, len(rows) * 0.55)
    fig, ax = plt.subplots(figsize=(fig_width, 4.8))
    ax.bar(positions, values, color=colors)
    ax.set_xticks(positions)
    ax.set_xticklabels(labels, rotation=45, ha="right")
    ax.set_xlabel("Instruction")
    ax.set_ylabel(ylabel)
    ax.set_title("Average miss-case latency")
    ax.grid(True, axis="y", linestyle="--", linewidth=0.5, alpha=0.5)
    fig.tight_layout()
    fig.savefig(os.path.join(output_dir, output_name), dpi=300)
    plt.close(fig)


def main():
    args = parse_args()
    if len(ARCHES) != len(CORES):
        raise SystemExit("ARCHES and CORES must have the same length")
    if args.core is None:
        args.core = core_for_arch(args.arch)

    args.output_dir = os.path.abspath(args.output_dir)
    os.makedirs(args.output_dir, exist_ok=True)
    filename_tag = make_filename_tag(args)
    csv_path = os.path.abspath(args.csv) if args.csv else os.path.join(
        args.output_dir, f"test-load-vs-sw-latency-{filename_tag}-results.csv"
    )

    if not args.plot_only:
        if not args.no_build:
            build_binary(args)
        run_test(args, csv_path)

    rows = read_csv(csv_path)
    plot(rows, args.output_dir, args.baseline_subtracted, filename_tag)

    print(f"Results CSV written to {csv_path}")
    print(f"Figures written under {args.output_dir}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
