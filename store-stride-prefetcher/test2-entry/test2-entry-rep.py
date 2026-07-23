#!/usr/bin/env python3
import argparse
import csv
import os
import re
import subprocess
import sys

ROOT_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
if ROOT_DIR not in sys.path:
    sys.path.insert(0, ROOT_DIR)

from cross_test_config import ARCH_CONFIG, arch_choices, is_x86_arch


BASE_DIR = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.join(BASE_DIR, "test2-entry-rep.c")
UTIL_SRC = os.path.join(ROOT_DIR, "until.c")
OUT = os.path.join(ROOT_DIR, "bin", "test2-entry-rep")
RESULT_DIR = os.path.join(ROOT_DIR, "res", "entry-rep")

DEFAULT_STRIDE_LINES = 5
DEFAULT_ROUNDS = 40000
INITIALIZED_ENTRIES = 16
BATCH_LAST_TEST_PAGE = 11
PMU_RE = re.compile(
    r"^# PMU\s+(?P<event>\S+)\s+per_round=(?P<per_round>[0-9]+(?:\.[0-9]+)?)$"
)
LATENCY_RE = re.compile(
    r"^\s*(?P<position>\d+)\s+(?P<offset>\d+)\s+"
    r"(?P<average>\d+)\s+(?P<probes>\d+)\s*$"
)


def parse_args():
    parser = argparse.ArgumentParser(
        description=(
            "Initialize sixteen store-stride entries and test one entry, or "
            "test pages 0 through 11 when --test-page is omitted."
        )
    )
    parser.add_argument("--arch", required=True, choices=arch_choices())
    parser.add_argument(
        "--core", type=int, default=None,
        help="CPU core used by taskset. Default is selected from --arch.",
    )
    parser.add_argument(
        "--test-page", type=int, default=None,
        help="Entry/page to test (0 through 15). Omit to test pages 0 through 11.",
    )
    parser.add_argument(
        "--stride", type=int, default=DEFAULT_STRIDE_LINES,
        help=f"Stride in cache lines. Default: {DEFAULT_STRIDE_LINES}",
    )
    parser.add_argument(
        "--rounds", type=int, default=DEFAULT_ROUNDS,
        help=f"Experiment rounds. Default: {DEFAULT_ROUNDS}",
    )
    parser.add_argument(
        "--no-trigger", action="store_true",
        help="Skip the third access to the selected entry.",
    )
    parser.add_argument("--cc", default=os.environ.get("CC", "gcc"))
    parser.add_argument("--no-compile", action="store_true")
    parser.add_argument(
        "--output", default=None,
        help="Latency TSV path. Default: res/entry-rep/<configuration>.tsv",
    )
    parser.add_argument(
        "--pmu-output", default=None,
        help="PMU TSV path. Default: res/entry-rep/<configuration>-pmu.tsv",
    )
    parser.add_argument(
        "--raw-output", default=None,
        help="Raw program output path. Default: res/entry-rep/raw/<configuration>.txt",
    )
    parser.add_argument(
        "--summary-output", default=None,
        help="Final l2d_cache_hwprf summary TSV path.",
    )
    args = parser.parse_args()

    if args.core is None:
        args.core = ARCH_CONFIG[args.arch]["core"]
    if args.core < 0:
        parser.error("--core must be >= 0")
    if (args.test_page is not None and
            not 0 <= args.test_page < INITIALIZED_ENTRIES):
        parser.error(f"--test-page must be in [0, {INITIALIZED_ENTRIES - 1}]")
    if args.test_page is None and args.no_compile:
        parser.error("--no-compile requires a single --test-page")
    if args.test_page is None and any(
        (args.output, args.pmu_output, args.raw_output)
    ):
        parser.error(
            "--output, --pmu-output and --raw-output require a single --test-page"
        )
    if args.stride < 1:
        parser.error("--stride must be >= 1")
    if 3 * args.stride >= 64:
        parser.error("the predicted address (3 * stride) must fit in one 4KB page")
    if args.rounds < 1:
        parser.error("--rounds must be >= 1")
    return args


def result_stem(args):
    trigger = "no-trigger" if args.no_trigger else "trigger"
    return (
        f"{args.arch}-core{args.core}-entry-rep-page{args.test_page}"
        f"-entries{INITIALIZED_ENTRIES}-stride{args.stride}"
        f"-rounds{args.rounds}-{trigger}"
    )


def output_paths(args):
    stem = result_stem(args)
    latency = args.output or os.path.join(RESULT_DIR, f"{stem}.tsv")
    pmu = args.pmu_output or os.path.join(RESULT_DIR, f"{stem}-pmu.tsv")
    raw = args.raw_output or os.path.join(RESULT_DIR, "raw", f"{stem}.txt")
    return latency, pmu, raw


def summary_path(args):
    if args.summary_output:
        return args.summary_output
    trigger = "no-trigger" if args.no_trigger else "trigger"
    stem = (
        f"{args.arch}-core{args.core}-entry-rep-entries{INITIALIZED_ENTRIES}"
        f"-stride{args.stride}-rounds{args.rounds}-{trigger}-l2d-summary.tsv"
    )
    return os.path.join(RESULT_DIR, stem)


def compile_test(args):
    os.makedirs(os.path.dirname(OUT), exist_ok=True)
    command = [
        args.cc,
        "-std=gnu11",
        "-O0",
        "-static",
        f"-DSTRIDE_BYTES={args.stride * 64}",
        f"-DROUNDS={args.rounds}",
        f"-DTEST_PAGE={args.test_page}",
        f"-DNO_TRIGGER={int(args.no_trigger)}",
        f"-DPMU_CORE_X925={int(args.arch == 'X925')}",
        "-o",
        OUT,
        SRC,
        UTIL_SRC,
    ]
    if not is_x86_arch(args.arch):
        command.insert(1, "-march=armv8.5-a+predres")
    print("Compiling:", " ".join(command))
    return subprocess.run(command).returncode


def parse_output(output):
    pmu_rows = []
    latency_rows = []
    for line in output.splitlines():
        pmu_match = PMU_RE.match(line)
        if pmu_match:
            pmu_rows.append(
                {
                    "event": pmu_match.group("event"),
                    "per_round": float(pmu_match.group("per_round")),
                }
            )
            continue
        latency_match = LATENCY_RE.match(line)
        if latency_match:
            latency_rows.append(
                {
                    "position": int(latency_match.group("position")),
                    "offset_bytes": int(latency_match.group("offset")),
                    "avg_ticks": int(latency_match.group("average")),
                    "probes": int(latency_match.group("probes")),
                }
            )
    return pmu_rows, latency_rows


def write_results(args, pmu_rows, latency_rows, latency_path, pmu_path):
    trigger = int(not args.no_trigger)
    os.makedirs(os.path.dirname(os.path.abspath(latency_path)), exist_ok=True)
    os.makedirs(os.path.dirname(os.path.abspath(pmu_path)), exist_ok=True)

    with open(latency_path, "w", newline="") as output:
        writer = csv.DictWriter(
            output,
            fieldnames=[
                "test_page", "trigger", "position", "offset_bytes",
                "avg_ticks", "probes",
            ],
            delimiter="\t",
        )
        writer.writeheader()
        for row in latency_rows:
            writer.writerow(
                {"test_page": args.test_page, "trigger": trigger, **row}
            )

    with open(pmu_path, "w", newline="") as output:
        writer = csv.DictWriter(
            output,
            fieldnames=["test_page", "trigger", "event", "per_round"],
            delimiter="\t",
        )
        writer.writeheader()
        for row in pmu_rows:
            writer.writerow(
                {"test_page": args.test_page, "trigger": trigger, **row}
            )


def run_one(args):
    latency_path, pmu_path, raw_path = output_paths(args)

    if not args.no_compile and compile_test(args) != 0:
        return 1, None
    if not os.path.isfile(OUT):
        print(f"Error: executable not found: {OUT}", file=sys.stderr)
        return 1, None

    print(
        f"Running: arch={args.arch}, core={args.core}, test_page={args.test_page}, "
        f"fixed_pages=20, initialized_entries={INITIALIZED_ENTRIES}, "
        f"initial_accesses=2, "
        f"trigger={not args.no_trigger}, stride={args.stride} lines, "
        f"rounds={args.rounds}"
    )
    run = subprocess.run(
        ["taskset", "-c", str(args.core), OUT],
        capture_output=True,
        text=True,
    )
    if run.stderr:
        print(run.stderr, file=sys.stderr, end="")

    os.makedirs(os.path.dirname(os.path.abspath(raw_path)), exist_ok=True)
    with open(raw_path, "w") as output:
        output.write(run.stdout)

    for line in run.stdout.splitlines():
        if line.startswith("# PMU"):
            print(line)

    if run.returncode != 0:
        print(f"Error: experiment exited with status {run.returncode}", file=sys.stderr)
        print(f"Saved raw output to {raw_path}")
        return run.returncode, None

    pmu_rows, latency_rows = parse_output(run.stdout)
    if not latency_rows:
        print("Error: no latency result was parsed", file=sys.stderr)
        return 1, None
    write_results(args, pmu_rows, latency_rows, latency_path, pmu_path)

    l2 = next(
        (row["per_round"] for row in pmu_rows
         if row["event"] == "l2d_cache_hwprf"),
        None,
    )
    if l2 is None:
        print("Error: l2d_cache_hwprf was not reported", file=sys.stderr)
        return 1, None
    latency = latency_rows[0]
    print(
        f"test_page={args.test_page}: predicted_position={latency['position']} "
        f"avg_ticks={latency['avg_ticks']} probes={latency['probes']}"
        + (f" l2d_cache_hwprf={l2:.2f}/round" if l2 is not None else "")
    )
    print(f"Saved latency data to {latency_path}")
    print(f"Saved PMU data to {pmu_path}")
    print(f"Saved raw output to {raw_path}")
    return 0, l2


def write_summary(rows, path):
    os.makedirs(os.path.dirname(os.path.abspath(path)), exist_ok=True)
    with open(path, "w", newline="") as output:
        writer = csv.writer(output, delimiter="\t")
        writer.writerow(["test_page", "l2d_cache_hwprf / round"])
        for test_page, per_round in rows:
            writer.writerow([test_page, f"{per_round:.2f}"])


def main():
    args = parse_args()
    pages = (
        [args.test_page]
        if args.test_page is not None
        else list(range(BATCH_LAST_TEST_PAGE + 1))
    )
    rows = []

    for test_page in pages:
        args.test_page = test_page
        status, l2 = run_one(args)
        if status != 0:
            return status
        rows.append((test_page, l2))

    path = summary_path(args)
    write_summary(rows, path)
    print("\ntest_page\tl2d_cache_hwprf / round")
    for test_page, per_round in rows:
        print(f"{test_page}\t{per_round:.2f}")
    print(f"Saved final summary to {path}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
