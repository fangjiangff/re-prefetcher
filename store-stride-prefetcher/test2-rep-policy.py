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
SRC = os.path.join(BASE_DIR, "test2-rep-policy.c")
OUT = os.path.join(BASE_DIR, "bin", "test2-rep-policy")

DEFAULT_ACCESS_PC = "0x500000120"
DEFAULT_VICTIM_BUFFER = "0x600000000"
DEFAULT_STRIDE_LINES = 5
DEFAULT_ENTRIES = 18
DEFAULT_EXTRA_COMPETITORS = 1
DEFAULT_ROUNDS = 1000
DEFAULT_PAGE_STEP = 1
DEFAULT_PROBE_POSITIONS = 100


def parse_args():
    parser = argparse.ArgumentParser(
        description="Run store-stride replacement-policy FIFO refresh test."
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
                        help="Total victim train+trigger accesses. Default is selected from --arch and --access.")
    parser.add_argument("--train-accesses", type=int, default=None,
                        help="Deprecated alias for --accesses.")
    parser.add_argument("--trigger-accesses", type=int, default=1,
                        help="Number of trigger accesses at the end of the stride sequence.")
    parser.add_argument("--entries", type=int, default=DEFAULT_ENTRIES,
                        help=f"Assumed replacement-set entries. Default: {DEFAULT_ENTRIES}")
    parser.add_argument("--fill-competitors", type=int, default=None,
                        help="Competitors inserted before optional victim refresh. Default: entries - 1.")
    parser.add_argument("--extra-competitors", type=int, default=DEFAULT_EXTRA_COMPETITORS,
                        help=f"Competitors inserted after optional victim refresh. Default: {DEFAULT_EXTRA_COMPETITORS}")
    parser.add_argument("--rounds", type=int, default=DEFAULT_ROUNDS)
    parser.add_argument("--page-step", type=int, default=DEFAULT_PAGE_STEP,
                        help="Distance between competitor pages, in 4KB pages.")
    parser.add_argument("--probe-lines", type=int, default=DEFAULT_PROBE_POSITIONS,
                        help=f"Number of victim cache-line positions available. Default: {DEFAULT_PROBE_POSITIONS}")
    parser.add_argument("--threshold-ns", type=int, default=None,
                        help="Latency threshold for summary classification. Default is selected from --arch.")
    parser.add_argument("--access", choices=["store", "load"], default="store",
                        help="Stride instruction to test. Default: store")
    parser.add_argument("--cc", default=os.environ.get("CC", "gcc"))
    parser.add_argument("--repeat-runs", type=int, default=1,
                        help="Run the experiment multiple times.")
    parser.add_argument("--output", default=None,
                        help="Combined TSV output path.")
    parser.add_argument("--summary-output", default=None,
                        help="Summary TSV output path.")
    parser.add_argument("--raw-output", default=None,
                        help="Raw stdout output path for single-run mode.")
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

    if args.fill_competitors is None:
        args.fill_competitors = args.entries - 1

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
    if args.entries < 2:
        parser.error("--entries must be >= 2")
    if args.fill_competitors < 0:
        parser.error("--fill-competitors must be >= 0")
    if args.extra_competitors < 0:
        parser.error("--extra-competitors must be >= 0")
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

    predicted_line = args.accesses * args.stride
    if predicted_line >= args.probe_lines:
        parser.error("--probe-lines must include the predicted line: accesses * stride")

    return args


args = parse_args()
result_dir = os.path.join(BASE_DIR, "res", "rep-policy")
raw_dir = os.path.join(result_dir, "raw")
multi_run_dir = os.path.join(result_dir, "multi-run")


def micro_arch_name():
    return (
        f"{args.arch}-core{args.core}-rep-policy"
        f"-stride{args.stride}-accesses{args.accesses}"
        f"-trigger{args.trigger_accesses}"
        f"-entries{args.entries}-fill{args.fill_competitors}"
        f"-extra{args.extra_competitors}-step{args.page_step}"
        f"-probe{args.probe_lines}-{args.access}"
    )


def combined_tsv_path():
    if args.output:
        return args.output
    if args.repeat_runs == 1:
        return os.path.join(result_dir, f"{micro_arch_name()}.tsv")
    return os.path.join(multi_run_dir, f"{micro_arch_name()}-runs{args.repeat_runs}.tsv")


def summary_tsv_path():
    if args.summary_output:
        return args.summary_output
    if args.repeat_runs == 1:
        return os.path.join(result_dir, f"{micro_arch_name()}-summary.tsv")
    return os.path.join(multi_run_dir, f"{micro_arch_name()}-runs{args.repeat_runs}-summary.tsv")


def raw_path(run_index):
    if args.raw_output and args.repeat_runs == 1:
        return args.raw_output
    if args.repeat_runs == 1:
        return os.path.join(raw_dir, f"{micro_arch_name()}.txt")
    return os.path.join(multi_run_dir, "raw", f"{micro_arch_name()}-run{run_index:02d}.txt")


def ensure_dirs():
    os.makedirs(os.path.dirname(OUT), exist_ok=True)
    for path in [combined_tsv_path(), summary_tsv_path(), raw_path(1)]:
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
        f"-DPROBE_POSITIONS={args.probe_lines}",
        f"-DTRAIN_ACCESS_LOAD={1 if args.access == 'load' else 0}",
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
            args.access_pc,
            args.victim_buffer,
            str(args.fill_competitors),
            str(args.extra_competitors),
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
        if len(fields) != 9:
            print(f"Skipping unexpected output row: {line}", file=sys.stderr)
            continue
        if fields[0] == "case":
            continue
        try:
            row = {
                "case": fields[0],
                "fill_competitors": int(fields[1]),
                "extra_competitors": int(fields[2]),
                "refresh_victim": int(fields[3]),
                "probe_line": int(fields[4]),
                "offset_bytes": int(fields[5]),
                "role": fields[6],
                "avg_ns": int(fields[7]),
                "probes": int(fields[8]),
            }
        except ValueError:
            print(f"Skipping non-numeric output row: {line}", file=sys.stderr)
            continue
        row["prefetched"] = "yes" if row["avg_ns"] <= args.threshold_ns else "no"
        row["threshold_ns"] = args.threshold_ns
        rows.append(row)
    return rows


def run_once(run_index):
    run = run_binary()
    path = raw_path(run_index)
    with open(path, "w") as f:
        f.write(run.stdout)
    if run.stderr:
        print(run.stderr, end="", file=sys.stderr)
    if run.returncode != 0:
        print(f"Saved raw output to {path}")
        print("Execution failed", file=sys.stderr)
        return run.returncode, []
    rows = parse_output(run.stdout)
    if not rows:
        print("No result rows parsed", file=sys.stderr)
        return 1, []
    for row in rows:
        if row.get("role") != "predicted":
            continue
        print(
            f"run={run_index} case={row['case']} role={row['role']} "
            f"probe_line={row['probe_line']} avg_ns={row['avg_ns']} "
            f"prefetched={row['prefetched']} threshold={args.threshold_ns}"
        )
    return 0, rows


def write_tsv(rows, path):
    with open(path, "w", newline="") as f:
        writer = csv.DictWriter(
            f,
            fieldnames=[
                "run",
                "case",
                "fill_competitors",
                "extra_competitors",
                "refresh_victim",
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


def by_case(rows, case_name):
    return [row for row in rows
            if row["case"] == case_name and row.get("role") == "predicted"]


def case_stats(rows, case_name):
    selected = by_case(rows, case_name)
    latencies = [row["avg_ns"] for row in selected]
    hits = sum(1 for row in selected if row["prefetched"] == "yes")
    total = len(selected)
    return {
        "case": case_name,
        "runs": total,
        "hit_count": hits,
        "hit_rate": hits / total if total else 0.0,
        "median_avg_ns": median(latencies) if latencies else "",
        "mean_avg_ns": f"{mean(latencies):.2f}" if latencies else "",
        "min_avg_ns": min(latencies) if latencies else "",
        "max_avg_ns": max(latencies) if latencies else "",
    }


def verdict(stats):
    no_trigger = stats["no_trigger"]
    baseline = stats["baseline"]
    refresh = stats["refresh"]

    if no_trigger["hit_rate"] > 0.25:
        return "inconclusive: no-trigger calibration is too often prefetched"
    filled = stats["filled"]
    if filled["hit_rate"] <= 0.5:
        return "inconclusive: filled baseline is not reliably prefetched; reduce --fill-competitors or adjust --entries"
    if baseline["hit_rate"] > 0.5:
        return "inconclusive: extra competitor does not reliably evict victim; increase --extra-competitors or adjust --entries"
    if refresh["hit_rate"] <= 0.5:
        return "FIFO-like: filled baseline works, but victim refresh did not protect against the extra competitor"
    return "not FIFO-like: victim refresh protected the entry, consistent with LRU/pseudo-LRU"


def write_summary(stats, decision):
    with open(summary_tsv_path(), "w", newline="") as f:
        writer = csv.DictWriter(
            f,
            fieldnames=[
                "case",
                "runs",
                "hit_count",
                "hit_rate",
                "median_avg_ns",
                "mean_avg_ns",
                "min_avg_ns",
                "max_avg_ns",
                "verdict",
            ],
            delimiter="\t",
        )
        writer.writeheader()
        for name in ["no_trigger", "filled", "baseline", "refresh"]:
            row = dict(stats[name])
            row["hit_rate"] = f"{row['hit_rate']:.3f}"
            row["verdict"] = decision if name == "refresh" else ""
            writer.writerow(row)


def print_header():
    print(
        f"arch={args.arch}, core={args.core}, stride={args.stride}, access={args.access}, "
        f"accesses={args.accesses}, train_only_accesses={args.train_only_accesses}, "
        f"trigger_accesses={args.trigger_accesses}, entries={args.entries}, "
        f"fill_competitors={args.fill_competitors}, extra_competitors={args.extra_competitors}, "
        f"rounds={args.rounds}, page_step={args.page_step}, threshold={args.threshold_ns} ns"
    )


def main():
    ensure_dirs()
    print_header()
    if not args.no_compile and compile_test() != 0:
        print("Compile failed", file=sys.stderr)
        return 1

    all_rows = []
    for run_index in range(1, args.repeat_runs + 1):
        print(f"# repeat_run={run_index}/{args.repeat_runs}")
        status, rows = run_once(run_index)
        if status != 0:
            return status
        for row in rows:
            combined = {"run": run_index}
            combined.update(row)
            all_rows.append(combined)

    write_tsv(all_rows, combined_tsv_path())
    stats = {name: case_stats(all_rows, name)
             for name in ["no_trigger", "filled", "baseline", "refresh"]}
    decision = verdict(stats)
    write_summary(stats, decision)

    print(f"Saved combined TSV to {combined_tsv_path()}")
    print(f"Saved summary TSV to {summary_tsv_path()}")
    for name in ["no_trigger", "filled", "baseline", "refresh"]:
        row = stats[name]
        print(
            f"{name}: hit_rate={row['hit_rate']:.3f} "
            f"median_avg_ns={row['median_avg_ns']} mean_avg_ns={row['mean_avg_ns']}"
        )
    print(decision)
    return 0


if __name__ == "__main__":
    sys.exit(main())
