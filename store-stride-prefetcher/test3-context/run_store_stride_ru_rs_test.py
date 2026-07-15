#!/usr/bin/env python3

import argparse
import csv
import os
import shlex
import subprocess
import sys
from datetime import datetime
from pathlib import Path

ROOT_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
if ROOT_DIR not in sys.path:
    sys.path.insert(0, ROOT_DIR)

from cross_test_config import (
    ARCH_CONFIG,
    apply_access_defaults,
    arch_choices,
    is_x86_arch,
)


BASE_DIR = Path(__file__).resolve().parent
SRC = BASE_DIR / "store_stride_ru_rs_test.c"
UTIL_SRC = Path(ROOT_DIR) / "until.c"
BIN_DIR = Path(ROOT_DIR) / "bin"
OUT = BIN_DIR / "store_stride_ru_rs_test"
RESULT_DIR = Path(ROOT_DIR) / "res" / "readunique-readshared"
RAW_DIR = RESULT_DIR / "raw"
DEFAULT_STRIDE_LINES = 5


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
    parser.add_argument("--access", choices=["store", "load"], default="store",
                        help="Access type used to select the architecture-specific "
                             "access count. Default: store")
    parser.add_argument("-s", "--stride", type=int, default=DEFAULT_STRIDE_LINES,
                        help="Stride in cache lines. Default: 5")
    parser.add_argument("--accesses", type=int, default=None,
                        help="Total train+trigger accesses. Default is selected "
                             "from --arch and --access.")
    parser.add_argument("--train-accesses", type=int, default=None,
                        help="Deprecated alias for --accesses.")
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
    args = parser.parse_args()
    if args.accesses is not None and args.train_accesses is not None:
        parser.error("use only one of --accesses or deprecated --train-accesses")
    if args.accesses is None and args.train_accesses is not None:
        args.accesses = args.train_accesses
    return args


def apply_arch_defaults(args):
    cross_core = ARCH_CONFIG[args.arch]["cross_core"]
    if args.observer_core is None:
        args.observer_core = cross_core["trigger_core"]
    if args.prefetcher_core is None:
        args.prefetcher_core = cross_core["train_core"]
    apply_access_defaults(args)


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
    if args.stride < 1:
        raise SystemExit("--stride must be >= 1")
    if args.accesses < 1:
        raise SystemExit("--accesses must be >= 1")


def default_name(args):
    stamp = datetime.now().strftime("%Y%m%d-%H%M%S")
    return (
        f"{args.arch}-ru-rs-observer{args.observer_core}"
        f"-prefetcher{args.prefetcher_core}"
        f"-{args.access}-stride{args.stride}-accesses{args.accesses}"
        f"-n{args.iterations}-w{args.warmup}-d{args.delay_nops}"
        f"-{stamp}"
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
    arch_cflags = [] if is_x86_arch(args.arch) else ["-march=armv8.5-a+predres"]
    cmd = [
        args.cc,
        *shlex.split(args.cflags),
        *arch_cflags,
        f"-DSTRIDE_LINES={args.stride}",
        f"-DTRAIN_ACCESSES={args.accesses}",
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
        "-s", "64",
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
            elif stripped in {
                "# core1_explicit_store_target_probe_avg_latency_ns",
                "# core1_explicit_store30_probe_avg_latency_ns",
            }:
                section = "core1_explicit_store_target_probe"
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
        f"access={args.access}, accesses={args.accesses}, "
        f"train_only_accesses={max(args.accesses - 1, 0)}, "
        f"trigger_accesses=1, stride_lines={args.stride}, "
        f"sequence={','.join(str(step * args.stride) for step in range(args.accesses))}, "
        f"target={args.accesses * args.stride}, "
        f"iterations={args.iterations}, warmup={args.warmup}, "
        f"delay_nops={args.delay_nops}"
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
