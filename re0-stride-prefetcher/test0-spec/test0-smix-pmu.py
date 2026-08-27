#!/usr/bin/env python3
"""Compile and run test0-smix-pmu.c, then save latency and PMU results."""

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

from cross_test_config import ARCH_CONFIG


SOURCE = BASE_DIR / "test0-smix-pmu.c"
BINARY_DIR = ROOT_DIR / "bin"
RESULT_DIR = ROOT_DIR / "res" / "store-stride-spec"
ACCESS_TYPES = ("load", "store")
PMU_ARCHES = ("X925", "A725", "A55")
ROW_RE = re.compile(r"^\s*(\d+)\s+(-?\d+)(?:\s+(-?\d+))?\s*$")
PMU_RE = re.compile(
    r"^# PMU\s+(?P<event>\S+)\s+per_round=(?P<value>[0-9]+(?:\.[0-9]+)?)\s*$"
)


def suffixed_path(path, suffix):
    return path.with_name(f"{path.stem}-{suffix}{path.suffix}")


def parse_args():
    parser = argparse.ArgumentParser(
        description=(
            "Compile and run test0-smix-pmu.c for load/store accesses, "
            "then save latency and PMU results."
        ),
        allow_abbrev=False,
    )
    parser.add_argument("--arch", required=True, choices=PMU_ARCHES)
    parser.add_argument(
        "--core", type=int, default=None,
        help="Override the CPU core. Default comes from cross_test_config.py.",
    )
    parser.add_argument("--cc", default=os.environ.get("CC", "gcc"))
    parser.add_argument("--train-times", type=int, default=15)
    parser.add_argument("--rounds", type=int, default=1)
    parser.add_argument("--tries", type=int, default=9999)
    parser.add_argument(
        "--secret-value", type=int, default=115,
        help="Secret byte value (default: 115, ASCII 's').",
    )
    parser.add_argument("--secret-len", type=int, default=1)
    parser.add_argument(
        "--type", choices=("load", "store", "both"), default="both",
        help="Victim access type to run (default: both).",
    )
    parser.add_argument(
        "--threshold", type=int, default=None,
        help="Cache-hit latency threshold in ns; default comes from --arch.",
    )
    parser.add_argument(
        "--pmu-device", default=None,
        help="Set PMU_DEVICE, for example armv8_pmuv3_1.",
    )
    parser.add_argument("--binary", type=Path, default=None)
    parser.add_argument("--data", type=Path, default=None,
                        help="Base path for latency TSV files.")
    parser.add_argument("--pmu-data", type=Path, default=None,
                        help="PMU TSV path containing all selected access types.")
    parser.add_argument("--raw-output", type=Path, default=None,
                        help="Base path for raw program output files.")
    parser.add_argument("--plot", type=Path, default=None)
    parser.add_argument("--no-compile", action="store_true")
    parser.add_argument("--no-plot", action="store_true")
    parser.add_argument("--show", action="store_true")
    parser.add_argument("--verbose", action="store_true")
    args = parser.parse_args()

    if args.core is None:
        args.core = ARCH_CONFIG[args.arch]["core"]
    if args.threshold is None:
        args.threshold = ARCH_CONFIG[args.arch]["threshold_ns"]
    if args.core < 0:
        parser.error("--core must be >= 0")
    if args.train_times < 0:
        parser.error("--train-times must be >= 0")
    if args.rounds < 1 or args.secret_len < 1:
        parser.error("--rounds and --secret-len must be positive")
    if args.tries < 256:
        parser.error("--tries must be at least 256 so every index is sampled")
    if not 0 <= args.secret_value <= 255:
        parser.error("--secret-value must be between 0 and 255")
    if args.threshold < 1:
        parser.error("--threshold must be positive")

    name = (
        f"test0-smix-pmu-{args.arch}-core{args.core}"
        f"-secret{args.secret_value}-train{args.train_times}"
        f"-rounds{args.rounds}-tries{args.tries}"
    )
    binary_base = args.binary or BINARY_DIR / "test0-smix-pmu"
    data_base = args.data or RESULT_DIR / f"{name}-latency.tsv"
    raw_base = args.raw_output or RESULT_DIR / "raw" / f"{name}.txt"
    args.pmu_data = args.pmu_data or RESULT_DIR / f"{name}-pmu.tsv"
    args.binaries = {
        access: suffixed_path(binary_base, access) for access in ACCESS_TYPES
    }
    args.data_paths = {
        access: suffixed_path(data_base, access) for access in ACCESS_TYPES
    }
    args.raw_paths = {
        access: suffixed_path(raw_base, access) for access in ACCESS_TYPES
    }
    args.access_types = ACCESS_TYPES if args.type == "both" else (args.type,)
    if args.plot is None:
        suffix = "load-store" if args.type == "both" else args.type
        args.plot = RESULT_DIR / "plots" / f"{name}-{suffix}.png"
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
        f"-DACCESS_TYPE_LOAD={int(access_type == 'load')}",
        f"-DPMU_CORE_X925={int(args.arch == 'X925')}",
        f"-DPMU_CORE_A55={int(args.arch == 'A55')}",
        str(SOURCE),
        "-o",
        str(binary),
    ]
    print("Compile:", " ".join(command))
    subprocess.run(command, check=True)


def run_program(args, binary):
    environment = os.environ.copy()
    if args.pmu_device:
        environment["PMU_DEVICE"] = args.pmu_device
    command = ["taskset", "-c", str(args.core), str(binary)]
    print("Run:", " ".join(command))
    return subprocess.run(
        command, check=True, capture_output=True, text=True, env=environment
    )


def parse_output(output):
    rows = []
    counters = {}
    notes = []
    in_table = False
    for line in output.splitlines():
        stripped = line.strip()
        columns = tuple(stripped.split())
        if columns in (("index", "latency"), ("index", "latency", "value")):
            in_table = True
            continue
        match = PMU_RE.match(stripped)
        if match:
            counters[match.group("event")] = float(match.group("value"))
            continue
        if stripped.startswith("# PMU unavailable") or stripped.startswith("# PMU note"):
            notes.append(stripped[2:])
        if in_table:
            match = ROW_RE.match(line)
            if match:
                value = int(match.group(3)) if match.group(3) is not None else None
                rows.append((int(match.group(1)), int(match.group(2)), value))

    indices = {index for index, _, _ in rows}
    if len(rows) != 256 or indices != set(range(256)):
        raise ValueError(f"expected indices 0..255, parsed {len(rows)} rows")
    return sorted(rows), counters, notes


def save_latency(path, rows):
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", newline="") as output_file:
        writer = csv.writer(output_file, delimiter="\t")
        writer.writerow(("index", "array2_offset", "latency_ns", "value"))
        writer.writerows(
            (index, index * 64, latency, "" if value is None else value)
            for index, latency, value in rows
        )


def save_pmu(path, counters_by_type):
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", newline="") as output_file:
        writer = csv.writer(output_file, delimiter="\t")
        writer.writerow(("access", "event", "per_victim_call"))
        for access_type, counters in counters_by_type.items():
            for event, value in sorted(counters.items()):
                writer.writerow((access_type, event, f"{value:.2f}"))


def save_raw(path, result):
    path.parent.mkdir(parents=True, exist_ok=True)
    text = result.stdout
    if result.stderr:
        text += "\n# stderr\n" + result.stderr
    path.write_text(text)


def plot_rows(path, rows_by_type, args):
    try:
        import matplotlib
        if not args.show:
            matplotlib.use("Agg")
        import matplotlib.pyplot as plt
    except ModuleNotFoundError as exc:
        raise RuntimeError(
            "matplotlib is required for plotting; use --no-plot to skip it"
        ) from exc

    path.parent.mkdir(parents=True, exist_ok=True)
    fig, axes = plt.subplots(
        len(args.access_types), 1,
        figsize=(10, 6.2 if args.type == "both" else 3.5),
        sharex=True, squeeze=False,
    )
    for ax, access_type in zip(axes[:, 0], args.access_types):
        rows = rows_by_type[access_type]
        indices = [row[0] for row in rows]
        latencies = [row[1] for row in rows]
        color = "#2166ac" if access_type == "load" else "#b2182b"
        ax.plot(indices, latencies, linewidth=1.4, color=color)
        ax.axhline(args.threshold, color="black", linestyle="--", linewidth=0.9,
                   label=f"threshold {args.threshold} ns")
        ax.axvline(args.secret_value, color="#e08214", linestyle=":",
                   linewidth=1.0, label=f"secret {args.secret_value}")
        ax.set_ylabel("latency (ns)")
        ax.set_title(access_type.capitalize(), pad=4)
        ax.set_xlim(0, 255)
        ax.grid(axis="y", color="0.88", linewidth=0.7)
        ax.legend(frameon=False)
        ax.spines["top"].set_visible(False)
        ax.spines["right"].set_visible(False)
    axes[-1, 0].set_xlabel("array2 index")
    fig.tight_layout()
    fig.savefig(path, dpi=200)
    if args.show:
        plt.show()
    plt.close(fig)


def print_pmu_summary(counters_by_type, notes_by_type):
    events = sorted({event for counters in counters_by_type.values() for event in counters})
    if events:
        print("\nPMU events per victim call:")
        print("\t".join(("access", *events)))
        for access_type, counters in counters_by_type.items():
            values = [f"{counters.get(event, 0.0):.2f}" for event in events]
            print("\t".join((access_type, *values)))
    else:
        print("\nPMU counters were unavailable; latency results were still saved.")
    for access_type, notes in notes_by_type.items():
        for note in notes:
            print(f"note ({access_type}): {note}")


def main():
    args = parse_args()
    if platform.machine().lower() not in {"aarch64", "arm64"}:
        print("error: test0-smix-pmu.c requires AArch64", file=sys.stderr)
        return 2

    try:
        rows_by_type = {}
        counters_by_type = {}
        notes_by_type = {}
        for access_type in args.access_types:
            binary = args.binaries[access_type]
            if not args.no_compile:
                compile_program(args, access_type, binary)
            result = run_program(args, binary)
            if args.verbose:
                print(result.stdout, end="")
                if result.stderr:
                    print(result.stderr, end="", file=sys.stderr)
            rows, counters, notes = parse_output(result.stdout)
            save_latency(args.data_paths[access_type], rows)
            save_raw(args.raw_paths[access_type], result)
            rows_by_type[access_type] = rows
            counters_by_type[access_type] = counters
            notes_by_type[access_type] = notes

        save_pmu(args.pmu_data, counters_by_type)
        if not args.no_plot:
            plot_rows(args.plot, rows_by_type, args)
    except (OSError, subprocess.CalledProcessError, ValueError, RuntimeError) as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1

    print_pmu_summary(counters_by_type, notes_by_type)
    for access_type in args.access_types:
        print(f"Saved {access_type} latency: {args.data_paths[access_type]}")
        print(f"Saved {access_type} raw output: {args.raw_paths[access_type]}")
    print(f"Saved PMU data: {args.pmu_data}")
    if not args.no_plot:
        print(f"Saved plot: {args.plot}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
