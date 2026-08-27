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
RESULT_DIR = ROOT_DIR / "res"
BINARY_DIR = ROOT_DIR / "bin"
ACCESS_TYPES = ("load", "store")
ROW_RE = re.compile(r"^\s*(\d+)\s+(-?\d+)\s*$")


def suffixed_path(path, suffix):
    return path.with_name(f"{path.stem}-{suffix}{path.suffix}")


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
    parser.add_argument(
        "--secret-value",
        type=int,
        default=115,
        help="Secret byte value (default: 115, ASCII 's').",
    )
    parser.add_argument("--secret-len", type=int, default=1)
    parser.add_argument(
        "--type",
        choices=("load", "store", "both"),
        default="both",
        help="Access type to run (default: both).",
    )
    parser.add_argument(
        "--threshold",
        type=int,
        help="Latency threshold; defaults to the value configured for --arch.",
    )
    parser.add_argument("--binary", type=Path)
    parser.add_argument("--data", type=Path)
    parser.add_argument("--plot", type=Path)
    parser.add_argument("--no-compile", action="store_true")
    parser.add_argument("--show", action="store_true", help="Open an interactive plot window.")
    args = parser.parse_args()

    args.core = ARCH_CONFIG[args.arch]["core"]
    if args.threshold is None:
        args.threshold = ARCH_CONFIG[args.arch]["threshold_ns"]
    if is_x86_arch(args.arch):
        parser.error(
            f"--arch {args.arch} is not supported because test0-smix.c uses "
            "AArch64-only instructions"
        )
    if args.train_times < 1 or args.rounds < 1 or args.secret_len < 1:
        parser.error("--train-times, --rounds and --secret-len must be positive")
    if args.threshold < 1:
        parser.error("--threshold must be positive")
    if args.tries < 256:
        parser.error("--tries must be at least 256 so every index is sampled")
    if not 0 <= args.secret_value <= 255:
        parser.error("--secret-value must be between 0 and 255")
    result_stem = f"test0-smix-{args.arch}-core{args.core}-latency"
    binary_base = args.binary or BINARY_DIR / "test0-smix"
    data_base = args.data or RESULT_DIR / f"{result_stem}.tsv"
    args.binaries = {
        access_type: suffixed_path(binary_base, access_type)
        for access_type in ACCESS_TYPES
    }
    args.data_paths = {
        access_type: suffixed_path(data_base, access_type)
        for access_type in ACCESS_TYPES
    }
    args.access_types = ACCESS_TYPES if args.type == "both" else (args.type,)
    if args.plot is None:
        plot_suffix = "load-store" if args.type == "both" else args.type
        args.plot = RESULT_DIR / f"{result_stem}-{plot_suffix}.png"
    return args


def compile_program(args, access_type, binary):
    binary.parent.mkdir(parents=True, exist_ok=True)
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
        f"-DACCESS_TYPE_LOAD={int(access_type == ACCESS_TYPES[0])}",
        str(SOURCE),
        "-o",
        str(binary),
    ]
    print("Compile:", " ".join(command))
    subprocess.run(command, check=True)


def run_program(args, binary):
    command = ["taskset", "-c", str(args.core), str(binary)]
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


def plot_rows(path, rows_by_type, args):
    try:
        import matplotlib
        if not args.show:
            matplotlib.use("Agg")
        import matplotlib.pyplot as plt
    except ModuleNotFoundError as exc:
        raise RuntimeError(
            "matplotlib is required: python3 -m pip install matplotlib"
        ) from exc

    path.parent.mkdir(parents=True, exist_ok=True)
    plt.rcParams.update(
        {
            "font.family": "serif",
            "font.size": 13,
            "axes.labelsize": 14,
            "axes.titlesize": 14,
            "xtick.labelsize": 12,
            "ytick.labelsize": 12,
            "legend.fontsize": 11,
        }
    )
    fig, axes = plt.subplots(
        len(args.access_types),
        1,
        figsize=(10, 6.2 if args.type == "both" else 3.5),
        sharex=True,
        squeeze=False,
    )
    axes = axes[:, 0]
    for ax, access_type in zip(axes, args.access_types):
        rows = rows_by_type[access_type]
        indices, latencies = zip(*rows)
        color = "#2166ac" if access_type == "load" else "#b2182b"
        ax.plot(indices, latencies, linewidth=1.4, color=color)
        for index, latency in rows:
            if latency < args.threshold:
                ax.annotate(
                    str(index),
                    xy=(index, latency),
                    xytext=(0, 4),
                    textcoords="offset points",
                    ha="center",
                    va="bottom",
                    fontsize=10,
                )
        ax.axhline(
            args.threshold,
            color="black",
            linestyle="--",
            linewidth=0.9,
            label=f"threshold {args.threshold} ns",
        )
        ax.set_ylabel("latency (ns)")
        ax.set_title(access_type.capitalize(), pad=4)
        ax.set_xlim(0, 255)
        ax.set_xticks(range(0, 256, 20))
        ax.grid(axis="y", color="0.88", linewidth=0.7)
        ax.legend(frameon=False)
        ax.spines["top"].set_visible(False)
        ax.spines["right"].set_visible(False)

    axes[-1].set_xlabel("index")
    fig.tight_layout(pad=0.8, h_pad=1.0)
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
        rows_by_type = {}
        for access_type in args.access_types:
            binary = args.binaries[access_type]
            data_path = args.data_paths[access_type]
            if not args.no_compile:
                compile_program(args, access_type, binary)
            output = run_program(args, binary)
            rows = parse_rows(output)
            save_data(data_path, rows)
            rows_by_type[access_type] = rows

        plot_rows(args.plot, rows_by_type, args)
    except (OSError, subprocess.CalledProcessError, ValueError, RuntimeError) as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1

    for access_type in args.access_types:
        print(f"Saved {access_type} data: {args.data_paths[access_type]}")
    print(f"Saved plot: {args.plot}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
