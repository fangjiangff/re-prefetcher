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
SRC = os.path.join(BASE_DIR, "test2-entry-af.c")
OUT = os.path.join(BASE_DIR, "bin", "test2-entry-af")

DEFAULT_ACCESS_PC = "0x500000120"
DEFAULT_BUFFER = "0x600000000"
DEFAULT_STRIDE_LINES = 5
DEFAULT_ENTRIES = 18
DEFAULT_ROUNDS = 1000
DEFAULT_PAGE_STEP = 1
DEFAULT_PROBE_POSITIONS = 64


def parse_args():
    parser = argparse.ArgumentParser(
        description="Run fixed-N AfterImage-style predicted-line latency test."
    )
    parser.add_argument("--arch", required=True, choices=arch_choices())
    parser.add_argument("--core", type=int, default=None,
                        help="Override CPU core. Default is selected from --arch.")
    parser.add_argument("--access-pc", default=DEFAULT_ACCESS_PC,
                        help=f"Fixed VA for the same-PC access gadget. Default: {DEFAULT_ACCESS_PC}")
    parser.add_argument("--store-pc", default=None,
                        help="Deprecated alias for --access-pc.")
    parser.add_argument("--buffer", default=DEFAULT_BUFFER,
                        help=f"Fixed VA for the stream-page buffer. Default: {DEFAULT_BUFFER}")
    parser.add_argument("--victim-buffer", default=None,
                        help="Deprecated alias for --buffer.")
    parser.add_argument("--stride", type=int, default=DEFAULT_STRIDE_LINES,
                        help=f"Stride in cache lines. Default: {DEFAULT_STRIDE_LINES}")
    parser.add_argument("--accesses", type=int, default=None,
                        help="Total train+trigger accesses per stream. Default is selected from --arch and --access.")
    parser.add_argument("--train-accesses", type=int, default=None,
                        help="Deprecated alias for --accesses.")
    parser.add_argument("--trigger-accesses", type=int, default=1,
                        help="Number of trigger accesses after training.")
    parser.add_argument("--entries", type=int, default=DEFAULT_ENTRIES,
                        help=f"Number of stream pages/prefetcher entries to test. Default: {DEFAULT_ENTRIES}")
    parser.add_argument("--max-entries", type=int, default=None,
                        help="Deprecated alias for --entries.")
    parser.add_argument("--rounds", type=int, default=DEFAULT_ROUNDS)
    parser.add_argument("--page-step", type=int, default=DEFAULT_PAGE_STEP,
                        help="Distance between tested stream pages, in 4KB pages.")
    parser.add_argument("--probe-lines", type=int, default=DEFAULT_PROBE_POSITIONS,
                        help=f"Number of cache-line positions reserved per page. Default: {DEFAULT_PROBE_POSITIONS}")
    parser.add_argument("--threshold-ns", type=int, default=None,
                        help="Latency threshold for prefetched classification. Default is selected from --arch.")
    parser.add_argument("--access", choices=["store", "load"], default="store",
                        help="Stride instruction to test. Default: store")
    parser.add_argument("--cc", default=os.environ.get("CC", "gcc"))
    parser.add_argument("--output", default=None,
                        help="TSV output path.")
    parser.add_argument("--raw-output", default=None,
                        help="Raw stdout path.")
    parser.add_argument("--summary-output", default=None,
                        help="Summary TSV path.")
    parser.add_argument("--no-compile", action="store_true")
    args = parser.parse_args()

    if args.store_pc is not None:
        args.access_pc = args.store_pc
    if args.victim_buffer is not None:
        args.buffer = args.victim_buffer
    if args.max_entries is not None:
        args.entries = args.max_entries

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
    if args.entries < 1:
        parser.error("--entries must be >= 1")
    if args.rounds < 1:
        parser.error("--rounds must be >= 1")
    if args.page_step < 1:
        parser.error("--page-step must be >= 1")
    if args.probe_lines < 1:
        parser.error("--probe-lines must be >= 1")
    if args.threshold_ns < 1:
        parser.error("--threshold-ns must be >= 1")

    predicted_line = args.accesses * args.stride
    if predicted_line >= args.probe_lines:
        parser.error("--probe-lines must include the predicted line: accesses * stride")

    return args


args = parse_args()
result_dir = os.path.join(BASE_DIR, "res", "entry-af")
raw_dir = os.path.join(result_dir, "raw")


def micro_arch_name():
    return (
        f"{args.arch}-core{args.core}-entry-af"
        f"-stride{args.stride}-accesses{args.accesses}"
        f"-trigger{args.trigger_accesses}"
        f"-entries{args.entries}-step{args.page_step}"
        f"-probe{args.probe_lines}-{args.access}"
    )


def tsv_path():
    if args.output:
        return args.output
    return os.path.join(result_dir, f"{micro_arch_name()}.tsv")


def summary_path():
    if args.summary_output:
        return args.summary_output
    return os.path.join(result_dir, f"{micro_arch_name()}-summary.tsv")


def raw_path():
    if args.raw_output:
        return args.raw_output
    return os.path.join(raw_dir, f"{micro_arch_name()}.txt")


def ensure_dirs():
    os.makedirs(os.path.dirname(OUT), exist_ok=True)
    for path in [tsv_path(), summary_path(), raw_path()]:
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
            args.buffer,
            str(args.entries),
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
        if fields[0] == "n":
            continue
        try:
            row = {
                "n": int(fields[0]),
                "page_index": int(fields[1]),
                "predicted_line": int(fields[2]),
                "offset_bytes": int(fields[3]),
                "avg_ns": int(fields[4]),
                "probes": int(fields[5]),
            }
        except ValueError:
            print(f"Skipping non-numeric output row: {line}", file=sys.stderr)
            continue
        row["prefetched"] = "yes" if row["avg_ns"] <= args.threshold_ns else "no"
        row["threshold_ns"] = args.threshold_ns
        rows.append(row)
    return rows


def write_tsv(rows):
    with open(tsv_path(), "w", newline="") as f:
        writer = csv.DictWriter(
            f,
            fieldnames=[
                "n",
                "page_index",
                "predicted_line",
                "offset_bytes",
                "avg_ns",
                "prefetched",
                "threshold_ns",
                "probes",
            ],
            delimiter="\t",
        )
        writer.writeheader()
        writer.writerows(rows)


def write_summary(rows):
    latencies = [row["avg_ns"] for row in rows]
    hits = sum(1 for row in rows if row["prefetched"] == "yes")
    total = len(rows)
    with open(summary_path(), "w", newline="") as f:
        writer = csv.DictWriter(
            f,
            fieldnames=[
                "n",
                "predicted_hits",
                "predicted_total",
                "hit_rate",
                "median_avg_ns",
                "mean_avg_ns",
                "min_avg_ns",
                "max_avg_ns",
            ],
            delimiter="\t",
        )
        writer.writeheader()
        writer.writerow({
            "n": args.entries,
            "predicted_hits": hits,
            "predicted_total": total,
            "hit_rate": f"{hits / total:.3f}" if total else "0.000",
            "median_avg_ns": median(latencies) if latencies else "",
            "mean_avg_ns": f"{mean(latencies):.2f}" if latencies else "",
            "min_avg_ns": min(latencies) if latencies else "",
            "max_avg_ns": max(latencies) if latencies else "",
        })
    return hits, total, latencies


def print_header():
    print(
        f"arch={args.arch}, core={args.core}, stride={args.stride}, access={args.access}, "
        f"accesses={args.accesses}, train_only_accesses={args.train_only_accesses}, "
        f"trigger_accesses={args.trigger_accesses}, entries={args.entries}, "
        f"rounds={args.rounds}, page_step={args.page_step}, probe_lines={args.probe_lines}, "
        f"threshold={args.threshold_ns} ns"
    )


def main():
    ensure_dirs()
    print_header()
    if not args.no_compile and compile_test() != 0:
        print("Compile failed", file=sys.stderr)
        return 1

    run = run_binary()
    with open(raw_path(), "w") as f:
        f.write(run.stdout)
    if run.stderr:
        print(run.stderr, end="", file=sys.stderr)
    if run.returncode != 0:
        print(f"Saved raw output to {raw_path()}")
        print("Execution failed", file=sys.stderr)
        return run.returncode

    rows = parse_output(run.stdout)
    if not rows:
        print("No result rows parsed", file=sys.stderr)
        return 1

    write_tsv(rows)
    hits, total, latencies = write_summary(rows)

    print("page_index\tavg_ns\tprefetched")
    for row in rows:
        print(f"{row['page_index']}\t{row['avg_ns']}\t{row['prefetched']}")
    print(f"Saved TSV to {tsv_path()}")
    print(f"Saved summary TSV to {summary_path()}")
    print(f"predicted hits={hits}/{total}, hit_rate={hits / total:.3f}")
    print(f"median_avg_ns={median(latencies)}, mean_avg_ns={mean(latencies):.2f}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
