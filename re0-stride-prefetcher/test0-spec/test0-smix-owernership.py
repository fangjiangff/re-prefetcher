#!/usr/bin/env python3
"""Compile and run the cross-core speculative-store ownership experiment."""

import argparse
import csv
import os
import platform
import re
import shlex
import subprocess
import sys
from pathlib import Path


BASE_DIR = Path(__file__).resolve().parent
ROOT_DIR = BASE_DIR.parent
if str(ROOT_DIR) not in sys.path:
    sys.path.insert(0, str(ROOT_DIR))

from cross_test_config import ARCH_CONFIG


SOURCE = BASE_DIR / "test0-smix-owernership.c"
DEFAULT_BINARY = ROOT_DIR / "bin" / "test0-smix-owernership"
RESULT_DIR = ROOT_DIR / "res" / "store-stride-spec"
ARCHES = ("X925", "A725", "A55")
MODES = (
    "transient_store_validation",
    "idle",
    "architectural_load",
    "architectural_store",
    "transient_no_access",
    "transient_load",
    "transient_store",
)
RESULT_RE = re.compile(
    r"^(?P<mode>\S+)\s+"
    r"(?P<avg>[0-9.]+)\s+(?P<p50>\d+)\s+(?P<p95>\d+)\s+"
    r"(?P<hit>[0-9.]+)\s+(?P<old>\d+)\s+(?P<new>\d+)\s+"
    r"(?P<other>\d+)\s+(?P<preload_bad>\d+)\s+"
    r"(?P<spec_avg>[0-9.]+)\s+(?P<spec_hit>[0-9.]+)\s*$"
)
PMU_RE = re.compile(
    r"^# PMU\s+(?P<event>\S+)\s+per_round=(?P<value>[0-9.]+)\s*$"
)


def parse_args():
    parser = argparse.ArgumentParser(
        description=(
            "Compile and run test0-smix-owernership.c, save the validation and six mode "
            "summaries, raw output, and PMU counters."
        ),
        allow_abbrev=False,
    )
    parser.add_argument("--arch", choices=ARCHES, default="X925")
    parser.add_argument("--actor-core", type=int)
    parser.add_argument("--observer-core", type=int)
    parser.add_argument("-n", "--iterations", type=int, default=10000)
    parser.add_argument("-w", "--warmup", type=int, default=1000)
    parser.add_argument("-t", "--train-times", type=int, default=15)
    parser.add_argument("-T", "--threshold-ns", type=int)
    parser.add_argument("-d", "--delay-nops", type=int, default=100)
    parser.add_argument("-r", "--repeats", type=int, default=1)
    parser.add_argument("--cc", default=os.environ.get("CC", "gcc"))
    parser.add_argument("--binary", type=Path, default=DEFAULT_BINARY)
    parser.add_argument("--output", type=Path,
                        help="Mode-summary TSV path.")
    parser.add_argument("--pmu-output", type=Path,
                        help="PMU TSV path.")
    parser.add_argument("--raw-output", type=Path,
                        help="Combined raw stdout/stderr path.")
    parser.add_argument("--pmu-device",
                        help="Override PMU_DEVICE, e.g. armv8_pmuv3_1.")
    parser.add_argument("--no-compile", action="store_true")
    parser.add_argument("--verbose", action="store_true")
    args = parser.parse_args()

    cross = ARCH_CONFIG[args.arch]["cross_core"]
    if args.actor_core is None:
        args.actor_core = cross["train_core"]
    if args.observer_core is None:
        args.observer_core = cross["trigger_core"]
    if args.threshold_ns is None:
        args.threshold_ns = ARCH_CONFIG[args.arch]["threshold_ns"]

    if args.actor_core < 0 or args.observer_core < 0:
        parser.error("CPU core numbers must be non-negative")
    if args.actor_core == args.observer_core:
        parser.error("actor and observer cores must be different")
    if args.iterations < 1 or args.threshold_ns < 1 or args.repeats < 1:
        parser.error("iterations, threshold, and repeats must be positive")
    if args.warmup < 0 or args.train_times < 0 or args.delay_nops < 0:
        parser.error("warmup, train-times, and delay-nops must be non-negative")

    stem = (
        f"ownership-{args.arch}-actor{args.actor_core}"
        f"-observer{args.observer_core}-n{args.iterations}"
    )
    args.output = args.output or RESULT_DIR / f"{stem}.tsv"
    args.pmu_output = args.pmu_output or RESULT_DIR / f"{stem}-pmu.tsv"
    args.raw_output = args.raw_output or RESULT_DIR / "raw" / f"{stem}.txt"
    return args


def compile_program(args):
    args.binary.parent.mkdir(parents=True, exist_ok=True)
    command = [
        args.cc,
        "-std=gnu11",
        "-O0",
        "-Wall",
        "-Wextra",
        "-pthread",
        f"-DPMU_CORE_X925={int(args.arch == 'X925')}",
        f"-DPMU_CORE_A55={int(args.arch == 'A55')}",
        str(SOURCE),
        "-o",
        str(args.binary),
    ]
    print("Compile:", shlex.join(command))
    subprocess.run(command, check=True)


def run_once(args):
    environment = os.environ.copy()
    if args.pmu_device:
        environment["PMU_DEVICE"] = args.pmu_device
    command = [
        str(args.binary),
        "-a", str(args.actor_core),
        "-b", str(args.observer_core),
        "-n", str(args.iterations),
        "-w", str(args.warmup),
        "-t", str(args.train_times),
        "-T", str(args.threshold_ns),
        "-d", str(args.delay_nops),
    ]
    print("Run:", shlex.join(command))
    return subprocess.run(
        command, check=True, capture_output=True, text=True, env=environment
    )


def parse_output(output, repeat):
    summaries = []
    pmu_rows = []
    pmu_mode = None

    for line in output.splitlines():
        stripped = line.strip()
        if stripped.startswith("# PMU_MODE "):
            pmu_mode = stripped.removeprefix("# PMU_MODE ").strip()
            continue
        pmu_match = PMU_RE.match(stripped)
        if pmu_match and pmu_mode in MODES:
            pmu_rows.append({
                "repeat": repeat,
                "mode": pmu_mode,
                "event": pmu_match.group("event"),
                "per_round": float(pmu_match.group("value")),
            })
            continue
        result_match = RESULT_RE.match(stripped)
        if result_match and result_match.group("mode") in MODES:
            summaries.append({
                "repeat": repeat,
                "mode": result_match.group("mode"),
                "avg_ns": float(result_match.group("avg")),
                "p50_ns": int(result_match.group("p50")),
                "p95_ns": int(result_match.group("p95")),
                "hit_rate": float(result_match.group("hit")),
                "old": int(result_match.group("old")),
                "new": int(result_match.group("new")),
                "other": int(result_match.group("other")),
                "preload_bad": int(result_match.group("preload_bad")),
                "spec_avg_ns": float(result_match.group("spec_avg")),
                "spec_hit_rate": float(result_match.group("spec_hit")),
            })

    found_modes = {row["mode"] for row in summaries}
    if found_modes != set(MODES):
        missing = sorted(set(MODES) - found_modes)
        raise ValueError(f"could not parse all result modes; missing: {missing}")
    return summaries, pmu_rows


def write_tsv(path, rows, fieldnames):
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", newline="") as output_file:
        writer = csv.DictWriter(output_file, fieldnames=fieldnames, delimiter="\t")
        writer.writeheader()
        writer.writerows(rows)


def print_summary(rows):
    print("\nrepeat  mode                    avg_ns  p95_ns  hit_rate  "
          "spec_hit  old  new")
    for row in rows:
        print(
            f"{row['repeat']:>6}  {row['mode']:<22}  "
            f"{row['avg_ns']:>6.2f}  {row['p95_ns']:>6}  "
            f"{row['hit_rate']:>8.4f}  {row['spec_hit_rate']:>8.4f}  "
            f"{row['old']:>5}  {row['new']:>5}"
        )


def main():
    args = parse_args()
    if platform.machine().lower() not in {"aarch64", "arm64"}:
        print("error: this experiment requires AArch64", file=sys.stderr)
        return 2

    try:
        if not args.no_compile:
            compile_program(args)
        elif not args.binary.is_file():
            raise FileNotFoundError(f"binary does not exist: {args.binary}")

        all_summaries = []
        all_pmu = []
        raw_parts = []
        for repeat in range(1, args.repeats + 1):
            result = run_once(args)
            summaries, pmu_rows = parse_output(result.stdout, repeat)
            all_summaries.extend(summaries)
            all_pmu.extend(pmu_rows)
            raw = f"===== repeat {repeat} stdout =====\n{result.stdout}"
            if result.stderr:
                raw += f"\n===== repeat {repeat} stderr =====\n{result.stderr}"
            raw_parts.append(raw.rstrip() + "\n")
            if args.verbose:
                print(raw_parts[-1], end="")

        write_tsv(
            args.output,
            all_summaries,
            ("repeat", "mode", "avg_ns", "p50_ns", "p95_ns", "hit_rate",
             "old", "new", "other", "preload_bad", "spec_avg_ns",
             "spec_hit_rate"),
        )
        write_tsv(
            args.pmu_output,
            all_pmu,
            ("repeat", "mode", "event", "per_round"),
        )
        args.raw_output.parent.mkdir(parents=True, exist_ok=True)
        args.raw_output.write_text("\n".join(raw_parts))
    except (OSError, subprocess.CalledProcessError, ValueError) as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1

    print_summary(all_summaries)
    print(f"\nSaved summary: {args.output}")
    print(f"Saved PMU:     {args.pmu_output}")
    print(f"Saved raw:     {args.raw_output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
