import argparse
import csv
import os
import subprocess
import sys

os.environ.setdefault("MPLCONFIGDIR", "/tmp/matplotlib-cache")
import matplotlib.pyplot as plt

from cross_test_config import (
    apply_single_core_defaults,
    apply_threshold_defaults,
    arch_choices,
)


BASE_DIR = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.join(BASE_DIR, "test1-index-same12-33-mem-pa.c")
UTIL_SRC = os.path.join(BASE_DIR, "until.c")
OUT = os.path.join(BASE_DIR, "bin", "test1-index-same12-33-mem-pa")

DEFAULT_BUDDY_MB = 64
DEFAULT_STRIDE_LINES = 5
DEFAULT_ROUNDS = 40000
DEFAULT_SCAN_LINES = 101
RESULT_DIR = os.path.join(BASE_DIR, "res", "index-same12-33-mem-pa")
PLOT_DIR = os.path.join(BASE_DIR, "res", "barplots")
FIELDS = ["region", "probe_pos", "offset_bytes", "probe_va", "probe_pa", "latency_ns", "probes"]


def compile_test(args):
    os.makedirs(os.path.dirname(OUT), exist_ok=True)
    compile_cmd = [
        args.cc,
        "-std=gnu11",
        "-O0",
        "-static",
        f"-DARCH_NAME=\"{args.arch}\"",
        f"-DBUDDY_PAGES={(args.buddy_mb * 1024 * 1024) // 4096}",
        f"-DSTRIDE_LINES={args.stride}",
        f"-DROUNDS={args.rounds}",
        f"-DSCAN_LINES={args.scan_lines}",
        "-o",
        OUT,
        SRC,
        UTIL_SRC,
    ]
    return subprocess.run(compile_cmd)


def run_test(args):
    run_cmd = ["taskset", "-c", str(args.core), OUT]
    return subprocess.run(run_cmd, capture_output=True, text=True)


def micro_arch_name(args):
    return (
        f"{args.arch}-core{args.core}-same12-33-mem-pa"
        f"-stride{args.stride}-scan{args.scan_lines}"
        f"-rounds{args.rounds}-buddy{args.buddy_mb}MB"
    )


def output_path(args):
    if args.output:
        return args.output
    return os.path.join(RESULT_DIR, f"{micro_arch_name(args)}.tsv")


def raw_path(args):
    if args.raw_output:
        return args.raw_output
    return os.path.join(RESULT_DIR, "raw", f"{micro_arch_name(args)}.txt")


def plot_path(args):
    if args.plot_output:
        return args.plot_output
    return os.path.join(PLOT_DIR, f"{micro_arch_name(args)}.png")


def ensure_parent(path):
    parent = os.path.dirname(path)
    if parent:
        os.makedirs(parent, exist_ok=True)


def parse_result(output):
    rows = []
    in_table = False

    for line in output.splitlines():
        stripped = line.strip()
        if not stripped or stripped.startswith("#"):
            continue
        parts = stripped.split()
        if parts == FIELDS:
            in_table = True
            continue
        if not in_table:
            continue
        if len(parts) != len(FIELDS):
            raise ValueError(f"unexpected result row: {line}")
        row = dict(zip(FIELDS, parts))
        row["probe_pos"] = int(row["probe_pos"])
        row["offset_bytes"] = int(row["offset_bytes"])
        row["latency_ns"] = int(row["latency_ns"])
        row["probes"] = int(row["probes"])
        rows.append(row)

    if not rows:
        raise ValueError("no result data found")
    return rows


def classify_rows(rows, threshold_ns):
    for row in rows:
        latency = row["latency_ns"]
        row["prefetched"] = (
            "no" if latency < 0
            else "yes" if latency <= threshold_ns
            else "no"
        )
        row["threshold_ns"] = threshold_ns


def write_tsv(path, rows):
    ensure_parent(path)
    with open(path, "w", newline="") as f:
        writer = csv.DictWriter(
            f,
            fieldnames=FIELDS + ["prefetched", "threshold_ns"],
            delimiter="\t",
        )
        writer.writeheader()
        writer.writerows(rows)


def plot_result(path, rows, title, threshold_ns, stride, vmin=None, vmax=None):
    ensure_parent(path)
    regions = ["PA1", "PA2"]
    all_values = [max(0, row["latency_ns"]) for row in rows]

    if vmin is None:
        vmin = 0
    if vmax is None:
        vmax = max(max(all_values) if all_values else 0, threshold_ns) * 1.12

    fig, axes = plt.subplots(2, 1, figsize=(13, 7.2), sharex=True)
    for ax, region in zip(axes, regions):
        region_rows = [row for row in rows if row["region"] == region]
        positions = [row["probe_pos"] for row in region_rows]
        values = [max(0, row["latency_ns"]) for row in region_rows]
        colors = []
        for row in region_rows:
            if row["probe_pos"] == stride:
                colors.append("#D55E00")
            elif row["probe_pos"] == 2 * stride:
                colors.append("#0072B2" if row["prefetched"] == "yes" else "#CC79A7")
            elif row["prefetched"] == "yes":
                colors.append("#009E73")
            else:
                colors.append("#BDBDBD")

        ax.bar(positions, values, color=colors, edgecolor="black", linewidth=0.2)
        ax.axhline(threshold_ns, color="black", linestyle="--", linewidth=1.0)
        ax.axvline(stride, color="#D55E00", linestyle=":", linewidth=1.1)
        ax.axvline(2 * stride, color="#0072B2", linestyle=":", linewidth=1.1)
        ax.set_title(region, loc="left", pad=4)
        ax.set_ylabel("latency (ns)")
        ax.set_ylim(vmin, vmax)
        ax.grid(axis="y", alpha=0.25)
        if positions:
            ax.set_xlim(min(positions) - 1, max(positions) + 1)
            tick_step = max(1, len(positions) // 20)
            ax.set_xticks(range(min(positions), max(positions) + 1, tick_step))

    axes[-1].set_xlabel("cache-line offset")
    fig.suptitle(title, x=0.01, ha="left")
    fig.tight_layout(rect=(0, 0, 1, 0.95))
    fig.savefig(path, dpi=300)
    plt.close(fig)


def main():
    parser = argparse.ArgumentParser(
        description="Validate whether PA[12:33]-matching pages share the store-stride prefetcher entry."
    )
    parser.add_argument("--arch", required=True, choices=arch_choices())
    parser.add_argument("--core", type=int, default=None,
                        help="Override CPU core. Default comes from --arch.")
    parser.add_argument("--buddy-mb", type=int, default=DEFAULT_BUDDY_MB,
                        help=f"Anonymous page pool size in MiB. Default: {DEFAULT_BUDDY_MB}")
    parser.add_argument("--stride", type=int, default=DEFAULT_STRIDE_LINES,
                        help="Stride in cache lines. Default: 5.")
    parser.add_argument("--rounds", type=int, default=DEFAULT_ROUNDS)
    parser.add_argument("--scan-lines", type=int, default=DEFAULT_SCAN_LINES,
                        help=f"Probe PA1/PA2+0..N-1 cache lines. Default: {DEFAULT_SCAN_LINES}")
    parser.add_argument("--threshold-ns", type=int, default=None,
                        help="Latency threshold for prefetched=yes. Default comes from --arch.")
    parser.add_argument("--cc", default=os.environ.get("CC", "gcc"))
    parser.add_argument("--output", default=None)
    parser.add_argument("--raw-output", default=None)
    parser.add_argument("--plot-output", default=None)
    parser.add_argument("--plot-vmin", type=float, default=None)
    parser.add_argument("--plot-vmax", type=float, default=200)
    parser.add_argument("--no-plot", action="store_true")
    parser.add_argument("--no-compile", action="store_true")
    args = parser.parse_args()

    apply_single_core_defaults(args)
    apply_threshold_defaults(args)

    if args.core < 0:
        parser.error("--core must be >= 0")
    if args.buddy_mb < 1:
        parser.error("--buddy-mb must be >= 1")
    if args.stride < 1:
        parser.error("--stride must be >= 1")
    if args.rounds < 1:
        parser.error("--rounds must be >= 1")
    if args.scan_lines < 1:
        parser.error("--scan-lines must be >= 1")
    if 2 * args.stride >= args.scan_lines:
        parser.error("predicted position 2 * stride must be inside scan lines")
    if args.threshold_ns < 1:
        parser.error("--threshold-ns must be >= 1")

    print(
        f"arch={args.arch}, core={args.core}, buddy_mb={args.buddy_mb}, "
        f"stride={args.stride}, trainer_pos=0, trigger_pos={args.stride}, "
        f"predicted_pos={2 * args.stride}, rounds={args.rounds}, "
        f"scan_lines={args.scan_lines}, threshold={args.threshold_ns} ns"
    )

    if not args.no_compile:
        res = compile_test(args)
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

    raw_result_path = raw_path(args)
    ensure_parent(raw_result_path)
    with open(raw_result_path, "w") as f:
        f.write(run.stdout)

    try:
        rows = parse_result(run.stdout)
    except ValueError as exc:
        print(f"Parse failed: {exc}", file=sys.stderr)
        return 1

    classify_rows(rows, args.threshold_ns)
    tsv_result_path = output_path(args)
    write_tsv(tsv_result_path, rows)
    print(f"Saved raw output to {raw_result_path}")
    print(f"Saved TSV to {tsv_result_path}")

    if not args.no_plot:
        plot_title = (
            f"{args.arch} core {args.core}: same PA[12:33] scan "
            f"(stride={args.stride}, buddy={args.buddy_mb}MB)"
        )
        plot_result(
            plot_path(args),
            rows,
            plot_title,
            args.threshold_ns,
            args.stride,
            vmin=args.plot_vmin,
            vmax=args.plot_vmax,
        )
        print(f"Saved plot to {plot_path(args)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
