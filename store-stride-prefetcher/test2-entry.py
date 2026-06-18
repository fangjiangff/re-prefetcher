import argparse
import csv
import os
import subprocess
import sys

from cross_test_config import (
    ARCH_CONFIG,
    apply_single_core_defaults,
    apply_threshold_defaults,
    apply_train_access_defaults,
    arch_choices,
)


BASE_DIR = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.join(BASE_DIR, "test2-entry.c")
OUT = os.path.join(BASE_DIR, "bin", "test2-entry")

DEFAULT_STORE_PC = "0x500000120"
DEFAULT_VICTIM_BUFFER = "0x600000000"
DEFAULT_STRIDE_LINES = 5
DEFAULT_MAX_COMPETITORS = 512
DEFAULT_ROUNDS = 1000
DEFAULT_PAGE_STEP = 1
DEFAULT_TRIGGER_LINE = 31
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
    parser.add_argument("--train-accesses", type=int, default=None,
                        help="Victim store accesses before competitor pressure. "
                             "Default is cross_test_config[arch]['train_accesses']['store'].")
    parser.add_argument("--max-competitors", type=int,
                        default=DEFAULT_MAX_COMPETITORS)
    parser.add_argument("--rounds", type=int, default=DEFAULT_ROUNDS)
    parser.add_argument("--page-step", type=int, default=DEFAULT_PAGE_STEP,
                        help="Distance between competitor pages, in 4KB pages.")
    parser.add_argument("--trigger-line", type=int,
                        default=DEFAULT_TRIGGER_LINE,
                        help=f"Victim trigger cache-line index. Default: {DEFAULT_TRIGGER_LINE}")
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
    parser.add_argument("--no-compile", action="store_true")
    args = parser.parse_args()

    args.access = "store"
    apply_single_core_defaults(args)
    apply_train_access_defaults(args)
    apply_threshold_defaults(args)

    if args.core < 0:
        parser.error("--core must be >= 0")
    if args.stride < 1:
        parser.error("--stride must be >= 1")
    if args.train_accesses < 1:
        parser.error("--train-accesses must be >= 1")
    if args.max_competitors < 0:
        parser.error("--max-competitors must be >= 0")
    if args.rounds < 1:
        parser.error("--rounds must be >= 1")
    if args.page_step < 1:
        parser.error("--page-step must be >= 1")
    if args.trigger_line < 0 or args.trigger_line >= 64:
        parser.error("--trigger-line must be in [0, 63]")
    if args.probe_lines < 1 or args.probe_lines > 64:
        parser.error("--probe-lines must be in [1, 64]")
    if args.threshold_ns < 1:
        parser.error("--threshold-ns must be >= 1")

    predicted_line = args.train_accesses * args.stride
    if predicted_line >= 64:
        parser.error("train_accesses * stride must keep predicted line inside one 4KB page")
    # if args.trigger_line == predicted_line:
    #     parser.error("--trigger-line must not equal the predicted/probe line")

    return args


args = parse_args()
result_dir = os.path.join(BASE_DIR, "res", "entry")
raw_dir = os.path.join(result_dir, "raw")


def micro_arch_name():
    return (
        f"{args.arch}-core{args.core}-entry"
        f"-stride{args.stride}-train{args.train_accesses}"
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


def ensure_dirs():
    os.makedirs(os.path.dirname(OUT), exist_ok=True)
    os.makedirs(result_dir, exist_ok=True)
    os.makedirs(raw_dir, exist_ok=True)
    for path in (tsv_path(), raw_path()):
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
        f"-DTRAIN_ACCESSES={args.train_accesses}",
        f"-DVICTIM_TRIGGER_LINE={args.trigger_line}",
        f"-DPROBE_LINES={args.probe_lines}",
        "-o",
        OUT,
        SRC,
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


def write_tsv(rows):
    with open(tsv_path(), "w", newline="") as f:
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


def main():
    ensure_dirs()
    print(
        f"arch={args.arch}, core={args.core}, stride={args.stride}, "
        f"train_accesses={args.train_accesses}, max_competitors={args.max_competitors}, "
        f"rounds={args.rounds}, page_step={args.page_step}, probe_lines={args.probe_lines}, "
        f"threshold={args.threshold_ns} ns"
    )
    print(
        "config default train_accesses="
        f"{ARCH_CONFIG[args.arch]['train_accesses']['store']}"
    )

    if not args.no_compile and compile_test() != 0:
        print("Compile failed", file=sys.stderr)
        return 1

    run = run_binary()
    if run.stdout:
        print(run.stdout, end="")
    if run.stderr:
        print(run.stderr, end="", file=sys.stderr)
    if run.returncode != 0:
        print("Execution failed", file=sys.stderr)
        return run.returncode

    with open(raw_path(), "w") as f:
        f.write(run.stdout)

    rows = parse_output(run.stdout)
    if not rows:
        print("No result rows parsed", file=sys.stderr)
        return 1

    write_tsv(rows)
    print(f"Saved TSV to {tsv_path()}")
    print(f"Saved raw output to {raw_path()}")

    evicted = first_eviction(rows)
    if evicted:
        print(
            "first victim non-prefetched point: "
            f"competitors={evicted['competitors']} probe_line={evicted['probe_line']} "
            f"avg_ns={evicted['avg_ns']}"
        )
    else:
        print("victim stayed below threshold for all tested competitors")

    return 0


if __name__ == "__main__":
    sys.exit(main())
