#!/usr/bin/env python3

import argparse
import csv
import os
import shlex
import subprocess
import sys
from datetime import datetime
from pathlib import Path

from cross_test_config import ARCH_CONFIG, arch_choices


BASE_DIR = Path(__file__).resolve().parent
SRC = BASE_DIR / "store_stride_ru_rs_test.c"
UTIL_SRC = BASE_DIR / "until.c"
BIN_DIR = BASE_DIR / "bin"
OUT = BIN_DIR / "store_stride_ru_rs_test"
RESULT_DIR = BASE_DIR / "res" / "readunique-readshared"
RAW_DIR = RESULT_DIR / "raw"


def parse_args():
    parser = argparse.ArgumentParser(
        description=(
            "Compile and run the cross-core store-stride ReadUnique vs "
            "ReadShared latency test."
        )
    )
    parser.add_argument("--arch", choices=arch_choices(), default="A55",
                        help="Architecture label used to select default cores. Default: A55")
    parser.add_argument("-a", "--observer-core", type=int, default=None,
                        help="Core that keeps/probes the target line. "
                             "Default is --arch cross_core.trigger_core.")
    parser.add_argument("-b", "--prefetcher-core", type=int, default=None,
                        help="Core that trains the store stride prefetcher. "
                             "Default is --arch cross_core.train_core.")
    parser.add_argument("-n", "--iterations", type=int, default=100000,
                        help="Measured iterations per mode. Default: 100000")
    parser.add_argument("-w", "--warmup", type=int, default=10000,
                        help="Warmup iterations per mode. Default: 10000")
    parser.add_argument("-d", "--delay-nops", type=int, default=100,
                        help="Nop delay after Core1 accesses. Default: 100")
    parser.add_argument("-s", "--elem-stride-bytes", type=int, default=64,
                        help="Bytes between logical elements. Default: 64")
    parser.add_argument("--cc", default=os.environ.get("CC", "gcc"),
                        help="Compiler command. Default: $CC or gcc")
    parser.add_argument("--cflags",
                        default="-std=gnu11 -O0 -static -Wall -Wextra -pthread",
                        help="Compiler flags as one shell-like string.")
    parser.add_argument("--output", default=None,
                        help="Parsed CSV output path.")
    parser.add_argument("--raw-output", default=None,
                        help="Raw program output path.")
    parser.add_argument("--no-compile", action="store_true",
                        help="Skip compilation and run the existing binary.")
    parser.add_argument("--compile-only", action="store_true",
                        help="Compile the binary but do not run it.")
    return parser.parse_args()


def apply_arch_defaults(args):
    cross_core = ARCH_CONFIG[args.arch]["cross_core"]
    if args.observer_core is None:
        args.observer_core = cross_core["trigger_core"]
    if args.prefetcher_core is None:
        args.prefetcher_core = cross_core["train_core"]


def validate_args(args):
    if args.observer_core < 0 or args.prefetcher_core < 0:
        raise SystemExit("core ids must be >= 0")
    if args.observer_core == args.prefetcher_core:
        raise SystemExit("observer and prefetcher cores must be different")
    if args.iterations < 1:
        raise SystemExit("--iterations must be >= 1")
    if args.warmup < 0:
        raise SystemExit("--warmup must be >= 0")
    if args.delay_nops < 0:
        raise SystemExit("--delay-nops must be >= 0")
    if args.elem_stride_bytes < 64:
        raise SystemExit("--elem-stride-bytes must be >= 64")


def default_name(args):
    stamp = datetime.now().strftime("%Y%m%d-%H%M%S")
    return (
        f"{args.arch}-ru-rs-observer{args.observer_core}"
        f"-prefetcher{args.prefetcher_core}"
        f"-n{args.iterations}-w{args.warmup}-d{args.delay_nops}"
        f"-s{args.elem_stride_bytes}-{stamp}"
    )


def apply_default_paths(args):
    name = default_name(args)
    if args.output is None:
        args.output = RESULT_DIR / f"{name}.csv"
    else:
        args.output = Path(args.output)
    if args.raw_output is None:
        args.raw_output = RAW_DIR / f"{name}.txt"
    else:
        args.raw_output = Path(args.raw_output)


def compile_test(args):
    BIN_DIR.mkdir(parents=True, exist_ok=True)
    cmd = [
        args.cc,
        *shlex.split(args.cflags),
        "-o",
        str(OUT),
        str(SRC),
        str(UTIL_SRC),
    ]
    print("+", " ".join(cmd))
    return subprocess.run(cmd)


def run_test(args):
    core_list = f"{args.observer_core},{args.prefetcher_core}"
    cmd = [
        "taskset",
        "-c",
        core_list,
        str(OUT),
        "-a", str(args.observer_core),
        "-b", str(args.prefetcher_core),
        "-n", str(args.iterations),
        "-w", str(args.warmup),
        "-d", str(args.delay_nops),
        "-s", str(args.elem_stride_bytes),
    ]
    print("+", " ".join(cmd))
    return subprocess.run(cmd, capture_output=True, text=True)


def parse_program_csv(stdout):
    rows = []
    section = None
    metadata = []

    for line in stdout.splitlines():
        stripped = line.strip()
        if not stripped:
            continue
        if stripped.startswith("#"):
            metadata.append(stripped)
            if stripped == "# cross_core_avg_latency_ns":
                section = "cross_core"
            elif stripped == "# core1_store_stride_prefetch_probe_avg_latency_ns":
                section = "core1_prefetch_probe"
            elif stripped == "# core1_explicit_store30_probe_avg_latency_ns":
                section = "core1_explicit_store30_probe"
            continue
        if section is None:
            raise ValueError(f"data row before section header: {line}")
        values = stripped.split(",")
        if len(values) != 2:
            raise ValueError(f"unexpected row: {line}")
        rows.append({
            "section": section,
            "name": values[0],
            "avg_ns": values[1],
        })

    if not rows:
        raise ValueError("no result rows found in program output")
    return metadata, rows


def write_outputs(args, stdout, stderr):
    args.raw_output.parent.mkdir(parents=True, exist_ok=True)
    args.raw_output.write_text(stdout + stderr, encoding="utf-8")

    metadata, rows = parse_program_csv(stdout)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    with args.output.open("w", newline="", encoding="utf-8") as f:
        for item in metadata:
            f.write(item + "\n")
        writer = csv.DictWriter(f, fieldnames=list(rows[0].keys()))
        writer.writeheader()
        writer.writerows(rows)


def main():
    args = parse_args()
    apply_arch_defaults(args)
    validate_args(args)
    apply_default_paths(args)

    print("=" * 60)
    print(
        f"arch={args.arch}, observer_core={args.observer_core}, "
        f"prefetcher_core={args.prefetcher_core}, "
        f"iterations={args.iterations}, warmup={args.warmup}, "
        f"delay_nops={args.delay_nops}, elem_stride_bytes={args.elem_stride_bytes}"
    )

    if not args.no_compile:
        result = compile_test(args)
        if result.returncode != 0:
            return result.returncode

    if args.compile_only:
        print(f"compiled: {OUT}")
        return 0

    result = run_test(args)
    sys.stdout.write(result.stdout)
    sys.stderr.write(result.stderr)
    if result.returncode != 0:
        return result.returncode

    write_outputs(args, result.stdout, result.stderr)
    print(f"raw: {args.raw_output}")
    print(f"csv: {args.output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
