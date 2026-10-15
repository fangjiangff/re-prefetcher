#!/usr/bin/env python3
"""Run train-left-offset forms as independent experiments."""

import argparse
import csv
import os
import platform
import shutil
import subprocess
from pathlib import Path

HERE = Path(__file__).resolve().parent
MODES = ("none", "single_pc_stride", "multi_pc_stride",
         "single_pc_page_pair", "multi_page_pair")


def make_parser():
    parser = argparse.ArgumentParser(
        description="Compare offset training forms after the standard training phase."
    )
    parser.add_argument("--binary", default=str(HERE / "test-bop-left-offset"))
    parser.add_argument("--cpu", type=int, default=6)
    parser.add_argument("--offset-lines", type=int, default=16)
    parser.add_argument("--train-pairs", type=int, default=300)
    parser.add_argument("--left-pairs", type=int, default=300)
    parser.add_argument("--rounds", type=int, default=30)
    parser.add_argument("--trigger-batch", type=int, default=64)
    parser.add_argument("--threshold-ns", type=int, default=80)
    parser.add_argument("--output")
    parser.add_argument("--build", action="store_true")
    parser.add_argument("--no-sudo", action="store_true")
    return parser


def mode_commands(binary, cpu, offset_lines, train_pairs, left_pairs, rounds,
                  trigger_batch, threshold_ns=80, use_sudo=False):
    prefix = [] if not use_sudo else ["sudo"]
    return [prefix + [
        "taskset", "-c", str(cpu), binary, "--mode", mode,
        "--cpu", str(cpu), "--offset-lines", str(offset_lines),
        "--train-pairs", str(train_pairs), "--left-pairs", str(left_pairs),
        "--rounds", str(rounds), "--trigger-batch", str(trigger_batch),
        "--threshold-ns", str(threshold_ns),
    ] for mode in MODES]


def parse_result(stdout):
    for line in stdout.splitlines():
        fields = line.split("\t")
        if len(fields) == 5 and fields[0].isdigit():
            return fields
    raise RuntimeError("result row not found")


def build_binary(binary):
    compiler = os.environ.get("CC")
    if compiler is None:
        compiler = "gcc" if platform.machine().lower() in ("aarch64", "arm64") else shutil.which("aarch64-linux-gnu-gcc")
    if compiler is None:
        raise RuntimeError("an AArch64 compiler is required")
    subprocess.run([
        compiler, "-std=gnu11", "-O2", "-Wall", "-Wextra", "-Werror",
        "-Wno-unused-result", "-march=armv8.5-a+predres",
        "-DENABLE_CPP_RCTX=1", str(HERE / "test-bop-left-offset.c"),
        str(HERE.parent / "re0-stride-prefetcher" / "until.c"), "-o", binary,
    ], check=True)


def main(argv=None):
    args = make_parser().parse_args(argv)
    if args.build:
        build_binary(args.binary)
    results = []
    for mode, command in zip(
        MODES,
        mode_commands(args.binary, args.cpu, args.offset_lines, args.train_pairs,
                      args.left_pairs, args.rounds, args.trigger_batch,
                      args.threshold_ns, not args.no_sudo),
    ):
        completed = subprocess.run(command, text=True, capture_output=True)
        print(completed.stdout, end="")
        if completed.returncode != 0:
            print(completed.stderr, end="")
            raise SystemExit(completed.returncode)
        row = parse_result(completed.stdout)
        results.append([mode] + row[1:])
    print("mode\tprobes\thits\thits_per_1000\tavg_latency_ns")
    for row in results:
        print("\t".join(row))
    if args.output:
        with open(args.output, "w", newline="") as stream:
            writer = csv.writer(stream, delimiter="\t", lineterminator="\n")
            writer.writerow(["mode", "probes", "hits", "hits_per_1000", "avg_latency_ns"])
            writer.writerows(results)


if __name__ == "__main__":
    main()
