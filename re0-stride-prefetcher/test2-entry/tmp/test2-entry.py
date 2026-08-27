import argparse
import csv
import os
import subprocess
import sys
from statistics import mean, median

ROOT_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
if ROOT_DIR not in sys.path:
    sys.path.insert(0, ROOT_DIR)

from cross_test_config import (
    apply_access_defaults,
    apply_single_core_defaults,
    apply_threshold_defaults,
    arch_choices,
)


BASE_DIR = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.join(BASE_DIR, "test2-entry.c")
OUT = os.path.join(ROOT_DIR, "bin", "test2-entry")

DEFAULT_ACCESS_PC = "0x500000120"
DEFAULT_VICTIM_BUFFER = "0x600000000"
DEFAULT_STRIDE_LINES = 5
DEFAULT_MAX_COMPETITORS = 64
DEFAULT_ROUNDS = 1000
DEFAULT_PAGE_STEP = 1
DEFAULT_PROBE_POSITIONS = 100


def parse_args():
    parser = argparse.ArgumentParser(
        description="Run test2-entry.c victim-retention/competitor eviction test."
    )
    parser.add_argument("--arch", required=True, choices=arch_choices())
    parser.add_argument("--core", type=int, default=None,
                        help="Override CPU core. Default is selected from --arch.")
    parser.add_argument("--access-pc", default=DEFAULT_ACCESS_PC,
                        help=f"Fixed VA for the same-PC access gadget. Default: {DEFAULT_ACCESS_PC}")
    parser.add_argument("--store-pc", default=None,
                        help="Deprecated alias for --access-pc.")
    parser.add_argument("--victim-buffer", default=DEFAULT_VICTIM_BUFFER,
                        help=f"Fixed VA for the victim page. Default: {DEFAULT_VICTIM_BUFFER}")
    parser.add_argument("--stride", type=int, default=DEFAULT_STRIDE_LINES,
                        help=f"Stride in cache lines. Default: {DEFAULT_STRIDE_LINES}")
    parser.add_argument("--accesses", type=int, default=None,
                        help="Total victim train+trigger accesses. "
                             "Default is selected from --arch and --access.")
    parser.add_argument("--train-accesses", type=int, default=None,
                        help="Deprecated alias for --accesses.")
    parser.add_argument("--trigger-accesses", type=int, default=1,
                        help="Number of trigger accesses at the end of the stride sequence.")
    parser.add_argument("--max-competitors", type=int,
                        default=DEFAULT_MAX_COMPETITORS)
    parser.add_argument("--rounds", type=int, default=DEFAULT_ROUNDS)
    parser.add_argument("--page-step", type=int, default=DEFAULT_PAGE_STEP,
                        help="Distance between competitor pages, in 4KB pages.")
    parser.add_argument("--probe-lines", type=int, default=DEFAULT_PROBE_POSITIONS,
                        help=f"Number of victim cache-line positions to probe after trigger. Default: {DEFAULT_PROBE_POSITIONS}")
    parser.add_argument("--threshold-ns", type=int, default=None,
                        help="Latency threshold for summary classification. "
                             "Default is selected from --arch.")
    parser.add_argument("--access", choices=["store", "load"], default="store",
                        help="Stride instruction to test. Default: store")
    parser.add_argument("--cc", default=os.environ.get("CC", "gcc"))
    parser.add_argument("--output", default=None,
                        help="TSV output path. Default is derived from arch/core/config.")
    parser.add_argument("--raw-output", default=None,
                        help="Raw stdout path. Default is derived from arch/core/config.")
    parser.add_argument("--plot-output", default=None,
                        help="Predicted probe-line latency plot path. Default is derived from arch/core/config.")
    parser.add_argument("--mutation-n", type=int, default=None,
                        help="Override the automatically detected first non-prefetched n marker.")
    parser.add_argument("--no-plot", action="store_true",
                        help="Do not draw the n-vs-probe-line-latency plot.")
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

    if args.store_pc is not None:
        args.access_pc = args.store_pc

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
    if args.probe_lines < 1:
        parser.error("--probe-lines must be >= 1")
    if args.threshold_ns < 1:
        parser.error("--threshold-ns must be >= 1")
    if args.repeat_runs < 1:
        parser.error("--repeat-runs must be >= 1")
    if args.mutation_n is not None and args.mutation_n < 0:
        parser.error("--mutation-n must be >= 0")
    if args.scatter_ymax <= args.threshold_ns:
        parser.error("--scatter-ymax must be greater than --threshold-ns")

    predicted_line = args.accesses * args.stride
    if predicted_line >= args.probe_lines:
        parser.error("--probe-lines must include the predicted line: accesses * stride")

    return args


args = parse_args()
result_dir = os.path.join(ROOT_DIR, "res", "entry")
raw_dir = os.path.join(result_dir, "raw")
multi_run_dir = os.path.join(result_dir, "multi-run")
plot_dir = os.path.join(result_dir, "plots")


def micro_arch_name():
    return (
        f"{args.arch}-core{args.core}-entry"
        f"-stride{args.stride}-accesses{args.accesses}"
        f"-trigger{args.trigger_accesses}"
        f"-max{args.max_competitors}-step{args.page_step}"
        f"-probe{args.probe_lines}"
        f"-{args.access}"
    )


def tsv_path():
    if args.output:
        return args.output
    return os.path.join(result_dir, f"{micro_arch_name()}.tsv")


def raw_path():
    if args.raw_output:
        return args.raw_output
    return os.path.join(raw_dir, f"{micro_arch_name()}.txt")


def plot_path():
    if args.plot_output:
        return args.plot_output
    return os.path.join(plot_dir, f"{micro_arch_name()}-predicted-line.png")


def run_plot_path(run_index):
    if args.repeat_runs == 1:
        return plot_path()
    return os.path.join(
        plot_dir,
        f"{micro_arch_name()}-run{run_index:02d}-predicted-line.png",
    )


def combined_plot_path():
    if args.plot_output:
        return args.plot_output
    return os.path.join(
        plot_dir,
        f"{micro_arch_name()}-runs{args.repeat_runs}-predicted-line.png",
    )


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
    paths = [tsv_path(), raw_path(), plot_path()]
    if args.repeat_runs > 1:
        paths.extend([combined_tsv_path(), summary_tsv_path(), scatter_path(),
                      combined_plot_path()])
        for i in range(1, args.repeat_runs + 1):
            paths.extend([run_tsv_path(i), run_raw_path(i), run_plot_path(i)])
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
        "-march=armv8.5-a+predres",
        f"-DSTRIDE_LINES={args.stride}",
        f"-DTRAIN_ACCESSES={args.train_only_accesses}",
        f"-DTRIGGER_ACCESSES={args.trigger_accesses}",
        f"-DPROBE_POSITIONS={args.probe_lines}",
        f"-DTRAIN_ACCESS_LOAD={1 if args.access == 'load' else 0}",
        "-o",
        OUT,
        SRC,
        os.path.join(ROOT_DIR, "until.c"),
    ]
    return subprocess.run(compile_cmd).returncode


def run_binary():
    return subprocess.run(
        [
            "taskset",
            "-c",
            str(args.core),
            OUT,
            args.access_pc,
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
        if (row["competitors"] >= 0 and
                row["role"] == "predicted" and
                row["prefetched"] == "no"):
            return row
    return None


def no_trigger_row(rows):
    for row in rows:
        if row["competitors"] == -1 and row["role"] == "predicted":
            return row
    return None


def print_no_trigger_check(rows):
    row = no_trigger_row(rows)
    if row is None:
        print("\033[31mERROR: no-trigger predicted row not found\033[0m")
        return None

    print(
        "no-trigger predicted latency: "
        f"probe_line={row['probe_line']} avg_ns={row['avg_ns']} "
        f"threshold={args.threshold_ns}"
    )
    if row["avg_ns"] <= args.threshold_ns:
        print(
            "\033[31m"
            "ERROR: no-trigger predicted latency must be greater than "
            f"hit-threshold-ns ({args.threshold_ns}); got {row['avg_ns']}"
            "\033[0m"
        )
    return row


def print_run_header():
    print(
        f"arch={args.arch}, core={args.core}, stride={args.stride}, "
        f"access={args.access}, "
        f"accesses={args.accesses}, train_only_accesses={args.train_only_accesses}, "
        f"trigger_accesses={args.trigger_accesses}, max_competitors={args.max_competitors}, "
        f"rounds={args.rounds}, page_step={args.page_step}, probe_lines={args.probe_lines}, "
        f"threshold={args.threshold_ns} ns"
    )


def print_probe_line_rows(rows):
    print("n\tprobe_line\toffset_bytes\trole\tavg_ns\tprobes")
    for row in rows:
        if row["role"] != "predicted":
            continue
        print(
            f"{row['competitors']}\t{row['probe_line']}\t"
            f"{row['offset_bytes']}\t{row['role']}\t"
            f"{row['avg_ns']}\t{row['probes']}"
        )


def predicted_rows(rows):
    return [row for row in rows
            if row["competitors"] >= 0 and row["role"] == "predicted"]


def plot_probe_line_latency(rows, path, title_suffix="", marker_row=None):
    if args.no_plot or not path:
        return
    try:
        import matplotlib.pyplot as plt
        from matplotlib.ticker import MaxNLocator
    except ModuleNotFoundError as exc:
        print(f"Skipping probe-line plot: missing Python package '{exc.name}'.")
        return

    plotted_rows = predicted_rows(rows)
    if not plotted_rows:
        print("Skipping probe-line plot: no predicted rows found.")
        return

    probe_line = plotted_rows[0]["probe_line"]
    fig, ax = plt.subplots(figsize=(9.5, 4.8))

    if any("run" in row for row in plotted_rows):
        by_run = {}
        for row in plotted_rows:
            by_run.setdefault(row.get("run", 1), []).append(row)

        all_xs = []
        all_ys = []
        for run_id in sorted(by_run):
            run_rows = sorted(by_run[run_id], key=lambda row: row["competitors"])
            xs = [row["competitors"] for row in run_rows]
            ys = [row["avg_ns"] for row in run_rows]
            all_xs.extend(xs)
            all_ys.extend(ys)
            ax.plot(xs, ys, marker="o", markersize=2.8, linewidth=1.1,
                    alpha=0.72, label=f"run {run_id}")
    else:
        by_n = {}
        for row in plotted_rows:
            by_n.setdefault(row["competitors"], []).append(row["avg_ns"])

        all_xs = sorted(by_n)
        all_ys = [mean(by_n[n]) for n in all_xs]
        ax.plot(all_xs, all_ys, marker="o", markersize=3.2, linewidth=1.4,
                color="#0072B2")
    ax.axhline(args.threshold_ns, color="black", linestyle="--",
               linewidth=1.0, label=f"threshold {args.threshold_ns} ns")
    marker_n = args.mutation_n
    marker_label = "manual"
    marker_latency = None
    if marker_n is None and marker_row is not None:
        marker_n = marker_row["competitors"]
        marker_label = "first non-prefetched"
        marker_latency = marker_row["avg_ns"]

    if marker_n is not None:
        ax.axvline(marker_n, color="#b30000", linestyle="--",
                   linewidth=1.3, label=f"{marker_label} n={marker_n}")
        label_y = marker_latency if marker_latency is not None else max(all_ys)
        ax.text(marker_n, label_y, f" n={marker_n}",
                color="#b30000", fontsize=14, va="bottom", ha="left")
    ax.set_title(
        f"Number of Competitors vs Access Latency{title_suffix}",
        loc="left",
        fontsize=16,
    )
    ax.set_xlabel("Number of Competitors (n)", fontsize=14)
    ax.set_ylabel("Access Latency", fontsize=14)
    if min(all_xs) == max(all_xs):
        ax.set_xlim(min(all_xs) - 1, max(all_xs) + 1)
    else:
        ax.set_xlim(min(all_xs), max(all_xs))
    ax.xaxis.set_major_locator(MaxNLocator(integer=True))
    ax.tick_params(axis="both", labelsize=14)
    ax.grid(axis="y", alpha=0.25)
    fig.tight_layout()
    fig.savefig(path, dpi=300)
    plt.close(fig)
    print(f"Saved probe-line latency plot to {path}")


def run_once(run_index):
    run = run_binary()

    raw_output_path = run_raw_path(run_index)
    with open(raw_output_path, "w") as f:
        f.write(run.stdout)

    if run.stderr:
        print(run.stderr, end="", file=sys.stderr)
    if run.returncode != 0:
        print(f"Saved raw output to {raw_output_path}")
        print("Execution failed", file=sys.stderr)
        return run.returncode, []

    rows = parse_output(run.stdout)
    if not rows:
        print("No result rows parsed", file=sys.stderr)
        return 1, []

    tsv_output_path = run_tsv_path(run_index)
    write_tsv(rows, tsv_output_path)
    print_probe_line_rows(rows)
    print(f"Saved TSV to {tsv_output_path}")
    print(f"Saved all probe latencies to {raw_output_path}")
    evicted = first_eviction(rows)
    plot_probe_line_latency(rows, run_plot_path(run_index),
                            marker_row=evicted)
    print_no_trigger_check(rows)
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
                "no_trigger_avg_ns",
                "no_trigger_valid",
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
        if row["competitors"] < 0 or row["role"] != "predicted":
            continue
        by_n.setdefault(row["competitors"], []).append(row["avg_ns"])

    for competitors in sorted(by_n):
        if median(by_n[competitors]) > args.threshold_ns:
            return competitors
    return None


def integer_median(values):
    if not values:
        return None

    ordered = sorted(values)
    mid = len(ordered) // 2
    if len(ordered) % 2 == 1:
        return ordered[mid]

    return (ordered[mid - 1] + ordered[mid] + 1) // 2


def repeated_marker_row(summary_rows):
    first_fail_ns = [
        row["first_fail_n"] for row in summary_rows
        if row["first_fail_n"] != ""
    ]
    marker_n = integer_median(first_fail_ns)
    if marker_n is None:
        return None

    matching_latencies = [
        row["first_fail_avg_ns"] for row in summary_rows
        if row["first_fail_n"] == marker_n and row["first_fail_avg_ns"] != ""
    ]

    marker_latency = integer_median(matching_latencies)
    if marker_latency is None:
        marker_latency = args.threshold_ns

    return {
        "competitors": marker_n,
        "avg_ns": marker_latency,
    }


def plot_scatter(rows, knee):
    try:
        import matplotlib.pyplot as plt
        from matplotlib.ticker import MaxNLocator
    except ModuleNotFoundError as exc:
        print(f"Skipping scatter plot: missing Python package '{exc.name}'.")
        return

    plotted_rows = [row for row in rows
                    if row["competitors"] >= 0 and row["role"] == "predicted"]
    xs = [row["competitors"] for row in plotted_rows]
    ys = [min(row["avg_ns"], args.scatter_ymax) for row in plotted_rows]

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
        f"Number of Competitors vs Access Latency ({args.repeat_runs} runs)",
        loc="left",
        fontsize=16,
    )
    ax.set_xlabel("Number of Competitors (n)", fontsize=14)
    ax.set_ylabel("Access Latency", fontsize=14)
    ax.set_xlim(0, args.max_competitors + 2)
    ax.set_ylim(0, args.scatter_ymax)
    ax.xaxis.set_major_locator(MaxNLocator(integer=True))
    ax.tick_params(axis="both", labelsize=14)
    ax.grid(axis="y", alpha=0.25)
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
        no_trigger = no_trigger_row(rows)
        latencies = [row["avg_ns"] for row in rows if row["competitors"] >= 0 and row["role"] == "predicted"]
        summary_rows.append({
            "run": run_index,
            "first_fail_n": evicted["competitors"] if evicted else "",
            "first_fail_avg_ns": evicted["avg_ns"] if evicted else "",
            "no_trigger_avg_ns": no_trigger["avg_ns"] if no_trigger else "",
            "no_trigger_valid": (
                "yes" if no_trigger and no_trigger["avg_ns"] > args.threshold_ns
                else "no"
            ),
            "min_avg_ns": min(latencies),
            "median_avg_ns": median(latencies),
            "mean_avg_ns": f"{mean(latencies):.2f}",
            "max_avg_ns": max(latencies),
        })

    write_combined_tsv(all_rows)
    write_summary_tsv(summary_rows)
    knee = median_knee(all_rows)
    marker = repeated_marker_row(summary_rows)
    plot_probe_line_latency(all_rows, combined_plot_path(),
                            f" ({args.repeat_runs} runs)",
                            marker_row=marker)
    plot_scatter(all_rows, knee)

    if marker is not None:
        print(
            "\033[32m"
            f"median first victim non-prefetched competitors={marker['competitors']}"
            "\033[0m"
        )
    else:
        print("median first victim non-prefetched point not found")

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
