#!/usr/bin/env python3
"""Compile, run and plot the index/latency output from test0-smix.c."""

import argparse
import csv
import os
import platform
import re
import subprocess
import sys
from pathlib import Path


BASE_DIR = Path(__file__).resolve().parent
ROOT_DIR = BASE_DIR.parent
if str(ROOT_DIR) not in sys.path:
    sys.path.insert(0, str(ROOT_DIR))

from cross_test_config import ARCH_CONFIG, arch_choices, is_x86_arch


SOURCE = BASE_DIR / "test0-smix.c"
DEFAULT_BINARY = BASE_DIR / "test0-smix"
ROW_RE = re.compile(r"^\s*(\d+)\s+(-?\d+)\s*$")


def parse_args():
    parser = argparse.ArgumentParser(
        description="Compile and run test0-smix.c, then plot index versus latency."
    )
    parser.add_argument(
        "--arch",
        required=True,
        choices=arch_choices(),
        help="Architecture name; its CPU core comes from cross_test_config.py.",
    )
    parser.add_argument("--cc", default=os.environ.get("CC", "gcc"))
    parser.add_argument("--train-times", type=int, default=15)
    parser.add_argument("--rounds", type=int, default=1)
    parser.add_argument("--tries", type=int, default=9999)
    parser.add_argument("--secret-value", type=int, default=8)
    parser.add_argument("--secret-len", type=int, default=1)
    parser.add_argument("--binary", type=Path, default=DEFAULT_BINARY)
    parser.add_argument("--data", type=Path)
    parser.add_argument("--plot", type=Path)
    parser.add_argument("--no-compile", action="store_true")
    parser.add_argument("--show", action="store_true", help="Open an interactive plot window.")
    args = parser.parse_args()

    args.core = ARCH_CONFIG[args.arch]["core"]
    if is_x86_arch(args.arch):
        parser.error(
            f"--arch {args.arch} is not supported because test0-smix.c uses "
            "AArch64-only instructions"
        )
    if args.train_times < 1 or args.rounds < 1 or args.secret_len < 1:
        parser.error("--train-times, --rounds and --secret-len must be positive")
    if args.tries < 256:
        parser.error("--tries must be at least 256 so every index is sampled")
    if not 0 <= args.secret_value <= 255:
        parser.error("--secret-value must be between 0 and 255")
    result_stem = f"test0-smix-{args.arch}-core{args.core}-latency"
    if args.data is None:
        args.data = BASE_DIR / f"{result_stem}.tsv"
    if args.plot is None:
        args.plot = BASE_DIR / f"{result_stem}.png"
    return args


def compile_program(args):
    args.binary.parent.mkdir(parents=True, exist_ok=True)
    command = [
        args.cc,
        "-std=gnu11",
        "-O0",
        "-Wall",
        "-Wextra",
        f"-DTRAIN_TIMES={args.train_times}",
        f"-DROUNDS={args.rounds}",
        f"-DTRY_COUNT={args.tries}",
        f"-DSECRET_VALUE={args.secret_value}",
        f"-DSECRET_LEN={args.secret_len}",
        str(SOURCE),
        "-o",
        str(args.binary),
    ]
    print("Compile:", " ".join(command))
    subprocess.run(command, check=True)


def run_program(args):
    command = ["taskset", "-c", str(args.core), str(args.binary)]
    print("Run:", " ".join(command))
    result = subprocess.run(command, check=True, capture_output=True, text=True)
    if result.stderr:
        print(result.stderr, end="", file=sys.stderr)
    return result.stdout


def parse_rows(output):
    rows = []
    in_table = False
    for line in output.splitlines():
        if line.strip() == "index\tlatency":
            in_table = True
            continue
        if not in_table:
            continue
        match = ROW_RE.match(line)
        if match:
            rows.append((int(match.group(1)), int(match.group(2))))

    indices = {index for index, _ in rows}
    if len(rows) != 256 or indices != set(range(256)):
        raise ValueError(f"expected indices 0..255, parsed {len(rows)} rows")
    return sorted(rows)


def save_data(path, rows):
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", newline="") as output_file:
        writer = csv.writer(output_file, delimiter="\t")
        writer.writerow(("index", "latency"))
        writer.writerows(rows)


def plot_rows(path, rows, args):
    try:
        import matplotlib
        if not args.show:
            matplotlib.use("Agg")
        import matplotlib.pyplot as plt
    except ModuleNotFoundError as exc:
        raise RuntimeError("matplotlib is required: python3 -m pip install matplotlib") from exc

    indices, latencies = zip(*rows)
    path.parent.mkdir(parents=True, exist_ok=True)
    fig, ax = plt.subplots(figsize=(12, 5))
    ax.plot(indices, latencies, linewidth=1.2, color="#3366aa")
    ax.set_xlabel("index")
    ax.set_ylabel("latency (ns)")
    ax.set_title(f"test0-smix ({args.arch}, core {args.core}): index vs latency")
    ax.set_xlim(0, 255)
    ax.grid(True, alpha=0.25)
    fig.tight_layout()
    fig.savefig(path, dpi=200)
    if args.show:
        plt.show()
    plt.close(fig)


def main():
    args = parse_args()
    machine = platform.machine().lower()
    if machine not in {"aarch64", "arm64"}:
        print("error: test0-smix.c uses AArch64-only cache instructions", file=sys.stderr)
        return 2

    try:
        if not args.no_compile:
            compile_program(args)
        output = run_program(args)
        rows = parse_rows(output)
        save_data(args.data, rows)
        plot_rows(args.plot, rows, args)
    except (OSError, subprocess.CalledProcessError, ValueError, RuntimeError) as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1

    print(f"Saved data: {args.data}")
    print(f"Saved plot: {args.plot}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
