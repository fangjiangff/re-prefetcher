import argparse
import csv
import os
import subprocess
import sys
from statistics import mean, median

from cross_test_config import (
    apply_access_defaults,
    apply_single_core_defaults,
    apply_threshold_defaults,
    arch_choices,
)


BASE_DIR = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.join(BASE_DIR, "test2-entry.c")
OUT = os.path.join(BASE_DIR, "bin", "test2-entry")

DEFAULT_STORE_PC = "0x500000120"
DEFAULT_VICTIM_BUFFER = "0x600000000"
DEFAULT_STRIDE_LINES = 5
DEFAULT_MAX_COMPETITORS = 64
DEFAULT_ROUNDS = 1000
DEFAULT_PAGE_STEP = 1
DEFAULT_PROBE_LINES = 64


def parse_args():
    parser = argparse.ArgumentParser(
        description="Run test2-entry.c victim-retention/competitor eviction test."
    )
    parser.add_argument("--arch", required=True, choices=arch_choices())
    parser.add_argument("--core", type=int, default=None,
                        help="Override CPU core. Default is selected from --arch.")
    parser.add_argument("--store-pc", default=DEFAULT_STORE_PC,
                        help=f"Fixed VA for the same-PC store gadget. Default: {DEFAULT_STORE_PC}")
    parser.add_argument("--victim-buffer", default=DEFAULT_VICTIM_BUFFER,
                        help=f"Fixed VA for the victim page. Default: {DEFAULT_VICTIM_BUFFER}")
    parser.add_argument("--stride", type=int, default=DEFAULT_STRIDE_LINES,
                        help=f"Stride in cache lines. Default: {DEFAULT_STRIDE_LINES}")
    parser.add_argument("--accesses", type=int, default=None,
                        help="Total victim train+trigger accesses. "
                             "Default is selected from --arch store accesses.")
    parser.add_argument("--train-accesses", type=int, default=None,
                        help="Deprecated alias for --accesses.")
    parser.add_argument("--trigger-accesses", type=int, default=1,
                        help="Number of trigger accesses at the end of the stride sequence.")
    parser.add_argument("--max-competitors", type=int,
                        default=DEFAULT_MAX_COMPETITORS)
    parser.add_argument("--rounds", type=int, default=DEFAULT_ROUNDS)
    parser.add_argument("--page-step", type=int, default=DEFAULT_PAGE_STEP,
                        help="Distance between competitor pages, in 4KB pages.")
    parser.add_argument("--probe-lines", type=int, default=DEFAULT_PROBE_LINES,
                        help=f"Number of victim cache lines to probe after trigger. Default: {DEFAULT_PROBE_LINES}")
    parser.add_argument("--threshold-ns", type=int, default=None,
                        help="Latency threshold for summary classification. "
                             "Default is selected from --arch.")
    parser.add_argument("--cc", default=os.environ.get("CC", "gcc"))
    parser.add_argument("--output", default=None,
                        help="TSV output path. Default is derived from arch/core/config.")
    parser.add_argument("--raw-output", default=None,
                        help="Raw stdout path. Default is derived from arch/core/config.")
    parser.add_argument("--repeat-runs", type=int, default=1,
                        help="Run the experiment multiple times and save a combined scatter plot.")
    parser.add_argument("--combined-output", default=None,
                        help="Combined TSV path for --repeat-runs > 1.")
    parser.add_argument("--summary-output", default=None,
                        help="Per-run summary TSV path for --repeat-runs > 1.")
    parser.add_argument("--scatter-output", default=None,
                        help="Scatter plot path for --repeat-runs > 1.")
    parser.add_argument("--scatter-ymax", type=int, default=300,
                        help="Clip scatter plot y values at this limit. Default: 300 ns.")
    parser.add_argument("--no-compile", action="store_true")
    args = parser.parse_args()

    args.access = "store"
    if args.accesses is not None and args.train_accesses is not None:
        parser.error("use only one of --accesses or deprecated --train-accesses")
    if args.accesses is None and args.train_accesses is not None:
        args.accesses = args.train_accesses

    apply_single_core_defaults(args)
    apply_access_defaults(args)
    apply_threshold_defaults(args)
    args.train_only_accesses = args.accesses - args.trigger_accesses

    if args.core < 0:
        parser.error("--core must be >= 0")
    if args.stride < 1:
        parser.error("--stride must be >= 1")
    if args.accesses < 1:
        parser.error("--accesses must be >= 1")
    if args.trigger_accesses < 1 or args.trigger_accesses > 2:
        parser.error("--trigger-accesses must be 1 or 2")
    if args.trigger_accesses >= args.accesses:
        parser.error("--trigger-accesses must be smaller than --accesses")
    if args.max_competitors < 0:
        parser.error("--max-competitors must be >= 0")
    if args.rounds < 1:
        parser.error("--rounds must be >= 1")
    if args.page_step < 1:
        parser.error("--page-step must be >= 1")
    if args.probe_lines < 1 or args.probe_lines > 64:
        parser.error("--probe-lines must be in [1, 64]")
    if args.threshold_ns < 1:
        parser.error("--threshold-ns must be >= 1")
    if args.repeat_runs < 1:
        parser.error("--repeat-runs must be >= 1")
    if args.scatter_ymax <= args.threshold_ns:
        parser.error("--scatter-ymax must be greater than --threshold-ns")

    predicted_line = args.accesses * args.stride
    if predicted_line >= 64:
        parser.error("train/trigger/predicted lines must fit in one 4KB page")

    return args


args = parse_args()
result_dir = os.path.join(BASE_DIR, "res", "entry")
raw_dir = os.path.join(result_dir, "raw")
multi_run_dir = os.path.join(result_dir, "multi-run")
plot_dir = os.path.join(result_dir, "plots")


def micro_arch_name():
    return (
        f"{args.arch}-core{args.core}-entry"
        f"-stride{args.stride}-accesses{args.accesses}"
        f"-trigger{args.trigger_accesses}"
        f"-max{args.max_competitors}-step{args.page_step}"
    )


def tsv_path():
    if args.output:
        return args.output
    return os.path.join(result_dir, f"{micro_arch_name()}.tsv")


def raw_path():
    if args.raw_output:
        return args.raw_output
    return os.path.join(raw_dir, f"{micro_arch_name()}.txt")


def combined_tsv_path():
    if args.combined_output:
        return args.combined_output
    return os.path.join(
        multi_run_dir,
        f"{micro_arch_name()}-runs{args.repeat_runs}.tsv",
    )


def summary_tsv_path():
    if args.summary_output:
        return args.summary_output
    return os.path.join(
        multi_run_dir,
        f"{micro_arch_name()}-runs{args.repeat_runs}-summary.tsv",
    )


def scatter_path():
    if args.scatter_output:
        return args.scatter_output
    return os.path.join(
        plot_dir,
        f"{micro_arch_name()}-runs{args.repeat_runs}-scatter.png",
    )


def run_tsv_path(run_index):
    if args.repeat_runs == 1:
        return tsv_path()
    return os.path.join(
        multi_run_dir,
        f"{micro_arch_name()}-run{run_index:02d}.tsv",
    )


def run_raw_path(run_index):
    if args.repeat_runs == 1:
        return raw_path()
    return os.path.join(
        multi_run_dir,
        "raw",
        f"{micro_arch_name()}-run{run_index:02d}.txt",
    )


def ensure_dirs():
    os.makedirs(os.path.dirname(OUT), exist_ok=True)
    os.makedirs(result_dir, exist_ok=True)
    os.makedirs(raw_dir, exist_ok=True)
    paths = [tsv_path(), raw_path()]
    if args.repeat_runs > 1:
        paths.extend([combined_tsv_path(), summary_tsv_path(), scatter_path()])
        for i in range(1, args.repeat_runs + 1):
            paths.extend([run_tsv_path(i), run_raw_path(i)])
    for path in paths:
        directory = os.path.dirname(path)
        if directory:
            os.makedirs(directory, exist_ok=True)


def compile_test():
    compile_cmd = [
        args.cc,
        "-std=gnu11",
        "-O0",
        "-static",
        f"-DSTRIDE_LINES={args.stride}",
        f"-DTRAIN_ACCESSES={args.train_only_accesses}",
        f"-DTRIGGER_ACCESSES={args.trigger_accesses}",
        f"-DPROBE_LINES={args.probe_lines}",
        "-o",
        OUT,
        SRC,
        os.path.join(BASE_DIR, "until.c"),
    ]
    return subprocess.run(compile_cmd).returncode


def run_binary():
    return subprocess.run(
        [
            "taskset",
            "-c",
            str(args.core),
            OUT,
            args.store_pc,
            args.victim_buffer,
            str(args.max_competitors),
            str(args.rounds),
            str(args.page_step),
        ],
        capture_output=True,
        text=True,
    )


def parse_output(output):
    rows = []

    for line in output.splitlines():
        stripped = line.strip()
        if not stripped or stripped.startswith("#"):
            continue

        fields = stripped.split()
        if len(fields) != 6:
            print(f"Skipping unexpected output row: {line}", file=sys.stderr)
            continue

        try:
            competitors = int(fields[0])
            probe_line = int(fields[1])
            offset_bytes = int(fields[2])
            role = fields[3]
            avg_ns = int(fields[4])
            probes = int(fields[5])
        except ValueError:
            print(f"Skipping non-numeric output row: {line}", file=sys.stderr)
            continue

        rows.append({
            "competitors": competitors,
            "probe_line": probe_line,
            "offset_bytes": offset_bytes,
            "role": role,
            "avg_ns": avg_ns,
            "prefetched": "yes" if avg_ns <= args.threshold_ns else "no",
            "threshold_ns": args.threshold_ns,
            "probes": probes,
        })

    return rows


def write_tsv(rows, path):
    with open(path, "w", newline="") as f:
        writer = csv.DictWriter(
            f,
            fieldnames=[
                "competitors",
                "probe_line",
                "offset_bytes",
                "role",
                "avg_ns",
                "prefetched",
                "threshold_ns",
                "probes",
            ],
            delimiter="\t",
        )
        writer.writeheader()
        writer.writerows(rows)


def first_eviction(rows):
    for row in rows:
        if row["role"] == "predicted" and row["prefetched"] == "no":
            return row
    return None


def print_run_header():
    print(
        f"arch={args.arch}, core={args.core}, stride={args.stride}, "
        f"accesses={args.accesses}, train_only_accesses={args.train_only_accesses}, "
        f"trigger_accesses={args.trigger_accesses}, max_competitors={args.max_competitors}, "
        f"rounds={args.rounds}, page_step={args.page_step}, probe_lines={args.probe_lines}, "
        f"threshold={args.threshold_ns} ns"
    )


def run_once(run_index):
    run = run_binary()
    if run.stdout:
        print(run.stdout, end="")
    if run.stderr:
        print(run.stderr, end="", file=sys.stderr)
    if run.returncode != 0:
        print("Execution failed", file=sys.stderr)
        return run.returncode, []

    raw_output_path = run_raw_path(run_index)
    with open(raw_output_path, "w") as f:
        f.write(run.stdout)

    rows = parse_output(run.stdout)
    if not rows:
        print("No result rows parsed", file=sys.stderr)
        return 1, []

    tsv_output_path = run_tsv_path(run_index)
    write_tsv(rows, tsv_output_path)
    print(f"Saved TSV to {tsv_output_path}")
    print(f"Saved raw output to {raw_output_path}")

    evicted = first_eviction(rows)
    if evicted:
        print(
            "\033[32m"
            "first victim non-prefetched point: "
            f"competitors={evicted['competitors']} probe_line={evicted['probe_line']} "
            f"avg_ns={evicted['avg_ns']}"
            "\033[0m"
        )
    else:
        print("victim stayed below threshold for all tested competitors")

    return 0, rows


def write_combined_tsv(rows):
    with open(combined_tsv_path(), "w", newline="") as f:
        writer = csv.DictWriter(
            f,
            fieldnames=[
                "run",
                "competitors",
                "probe_line",
                "offset_bytes",
                "role",
                "avg_ns",
                "prefetched",
                "threshold_ns",
                "probes",
            ],
            delimiter="\t",
        )
        writer.writeheader()
        writer.writerows(rows)


def write_summary_tsv(rows):
    with open(summary_tsv_path(), "w", newline="") as f:
        writer = csv.DictWriter(
            f,
            fieldnames=[
                "run",
                "first_fail_n",
                "first_fail_avg_ns",
                "min_avg_ns",
                "median_avg_ns",
                "mean_avg_ns",
                "max_avg_ns",
            ],
            delimiter="\t",
        )
        writer.writeheader()
        writer.writerows(rows)


def median_knee(rows):
    by_n = {}
    for row in rows:
        by_n.setdefault(row["competitors"], []).append(row["avg_ns"])

    for competitors in sorted(by_n):
        if median(by_n[competitors]) > args.threshold_ns:
            return competitors
    return None


def plot_scatter(rows, knee):
    try:
        import matplotlib.pyplot as plt
        from matplotlib.ticker import MaxNLocator
    except ModuleNotFoundError as exc:
        print(f"Skipping scatter plot: missing Python package '{exc.name}'.")
        return

    xs = [row["competitors"] for row in rows]
    ys = [min(row["avg_ns"], args.scatter_ymax) for row in rows]

    fig, ax = plt.subplots(figsize=(9.5, 4.8))
    ax.scatter(xs,
               ys,
               s=22,
               marker="D",
               color="#008b8b",
               alpha=0.72,
               edgecolors="none")
    ax.axhline(args.threshold_ns,
               color="black",
               linestyle="--",
               linewidth=1.2,
               label=f"threshold {args.threshold_ns} ns")
    if knee is not None:
        ax.axvline(knee,
                   color="#b30000",
                   linestyle="--",
                   linewidth=1.5,
                   label=f"median knee n={knee}")
        ax.text(knee,
                args.scatter_ymax * 0.92,
                str(knee),
                color="#b30000",
                fontsize=16,
                fontweight="bold",
                ha="center",
                va="center")
    ax.set_title(
        f"{args.arch} core {args.core}: test2-entry {args.repeat_runs} runs",
        loc="left",
    )
    ax.set_xlabel("Number of competitors (n)")
    ax.set_ylabel("Probe line average latency (ns)")
    ax.set_xlim(0, args.max_competitors + 2)
    ax.set_ylim(0, args.scatter_ymax)
    ax.xaxis.set_major_locator(MaxNLocator(integer=True))
    ax.grid(axis="y", alpha=0.25)
    ax.legend(loc="upper right")
    fig.tight_layout()
    fig.savefig(scatter_path(), dpi=300)
    plt.close(fig)
    print(f"Saved scatter plot to {scatter_path()}")


def run_repeated():
    all_rows = []
    summary_rows = []

    for run_index in range(1, args.repeat_runs + 1):
        print(f"# repeat_run={run_index}/{args.repeat_runs}")
        status, rows = run_once(run_index)
        if status != 0:
            return status

        for row in rows:
            combined_row = {"run": run_index}
            combined_row.update(row)
            all_rows.append(combined_row)

        evicted = first_eviction(rows)
        latencies = [row["avg_ns"] for row in rows]
        summary_rows.append({
            "run": run_index,
            "first_fail_n": evicted["competitors"] if evicted else "",
            "first_fail_avg_ns": evicted["avg_ns"] if evicted else "",
            "min_avg_ns": min(latencies),
            "median_avg_ns": median(latencies),
            "mean_avg_ns": f"{mean(latencies):.2f}",
            "max_avg_ns": max(latencies),
        })

    write_combined_tsv(all_rows)
    write_summary_tsv(summary_rows)
    knee = median_knee(all_rows)
    plot_scatter(all_rows, knee)

    print(f"Saved combined TSV to {combined_tsv_path()}")
    print(f"Saved summary TSV to {summary_tsv_path()}")
    if knee is not None:
        print(f"\033[32mmedian knee competitors={knee}\033[0m")
    else:
        print("median knee not found within tested competitors")

    return 0


def main():
    ensure_dirs()
    print_run_header()

    if not args.no_compile and compile_test() != 0:
        print("Compile failed", file=sys.stderr)
        return 1

    if args.repeat_runs > 1:
        return run_repeated()

    status, _ = run_once(1)
    return status


if __name__ == "__main__":
    sys.exit(main())
