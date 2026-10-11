#!/usr/bin/env python3

import argparse
import platform
import signal
import subprocess
import sys
from pathlib import Path


HERE = Path(__file__).resolve().parent
REPO = HERE.parent
SRC = HERE / "bop-cc-4bit.c"
UTIL_SRC = REPO / "re0-stride-prefetcher" / "until.c"
BIN_DIR = REPO / "bin"
RESULT_DIR = HERE / "res"
ARCH_CONFIG = {"X925": {"core": 6, "threshold_ns": 80},
               "A725": {"core": 4, "threshold_ns": 120}}


def default_compiler():
    if platform.machine().lower() in ("aarch64", "arm64"):
        return "gcc"
    return "aarch64-linux-gnu-gcc"


def parse_args(argv=None):
    parser = argparse.ArgumentParser(
        description=(
            "Run bop-cc-4bit: train a random offset in 1..32 and infer it "
            "with shuffled cache probes."
        )
    )
    parser.add_argument("--arch", choices=ARCH_CONFIG, default="X925")
    parser.add_argument("--core", "--cpu", dest="core", type=int)
    parser.add_argument("--rounds", type=int, default=1000)
    parser.add_argument("--threshold-ns", type=int)
    parser.add_argument("--train-pairs", type=int, default=1024)
    parser.add_argument("--seed", type=lambda value: int(value, 0),
                        default=0x243F6A8885A308D3)
    parser.add_argument("--cc", "--compiler", dest="cc",
                        default=default_compiler())
    parser.add_argument("--output", type=Path)
    parser.add_argument("--describe", action="store_true")
    parser.add_argument("--no-build", action="store_true")
    parser.add_argument("--no-run", "--build-only", dest="no_run", action="store_true")
    args = parser.parse_args(argv)
    config = ARCH_CONFIG[args.arch]
    if args.core is None:
        args.core = config["core"]
    if args.threshold_ns is None:
        args.threshold_ns = config["threshold_ns"]
    if args.core < 0 or args.rounds <= 0 or args.train_pairs <= 0:
        parser.error("core, rounds, and train-pairs must be positive")
    if args.threshold_ns <= 0:
        parser.error("threshold-ns must be positive")
    if not 0 <= args.seed < 1 << 64:
        parser.error("seed must fit in unsigned 64 bits")
    if args.train_pairs + (args.train_pairs + 6) // 7 + 1 > 8192:
        parser.error("training pages exceed the 8192-page pool")
    return args


def print_schedule(args):
    print(f"# arch={args.arch} core={args.core} measurement=cache-probe "
          f"random_value_range=1..32 rounds={args.rounds} "
          f"train_pairs={args.train_pairs} seed={args.seed}")
    print("# probe offsets=1..32; order is shuffled independently each round")


def build(args):
    output = BIN_DIR / "bop-cc-4bit"
    BIN_DIR.mkdir(parents=True, exist_ok=True)
    command = [
        args.cc, "-std=gnu11", "-O2", "-Wall", "-Wextra", "-Werror",
        "-Wno-unused-result", "-march=armv8.5-a+predres",
        "-DENABLE_CPP_RCTX=1", "-DENABLE_PMU=0", "-DENABLE_CACHE_PROBE=1",
        str(SRC), str(UTIL_SRC), "-o", str(output),
    ]
    subprocess.run(command, check=True)
    return output


def parse_output(text):
    parsed = {"error": {}, "bandwidth": {}}
    for line in text.splitlines():
        fields = line.split()
        if fields and fields[0] == "#":
            fields = fields[1:]
        if fields and fields[0].startswith("correct="):
            values = dict(item.split("=", 1) for item in fields)
            parsed["error"] = {
                key: float(value) for key, value in values.items()
            }
        elif fields and (fields[0].startswith("rounds_elapsed_ns=") or
                         fields[0].startswith("bits_transferred=")):
            values = dict(item.split("=", 1) for item in fields)
            parsed["bandwidth"] = {
                key: float(value) for key, value in values.items()
            }
    if not parsed["error"]:
        raise RuntimeError("bop-cc-4bit output has no error summary")
    return parsed


def run_binary(args, binary):
    if platform.machine().lower() not in ("aarch64", "arm64"):
        raise RuntimeError("execution requires an AArch64 host; use --no-run")
    command = ["taskset", "-c", str(args.core), str(binary), str(args.rounds),
               str(args.threshold_ns), str(args.train_pairs), str(args.seed)]
    completed = subprocess.run(command, text=True,
                               stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    if completed.returncode != 0:
        reason = (signal.Signals(-completed.returncode).name
                  if completed.returncode < 0 else f"status {completed.returncode}")
        raise RuntimeError(f"bop-cc-4bit failed with {reason}:\n{completed.stdout}")
    return completed.stdout, parse_output(completed.stdout)


def format_summary(parsed):
    bandwidth = parsed["bandwidth"]
    error = parsed["error"]
    return [
        (f"bandwidth_bit_s={bandwidth.get('bandwidth_bit_s', 0.0):.3f} "
         f"bandwidth_kbit_s={bandwidth.get('bandwidth_kbit_s', 0.0):.3f}"),
        f"error_rate_percent={error.get('error_rate_percent', 0.0):.3f}",
    ]


def print_summary(parsed):
    for line in format_summary(parsed):
        if sys.stdout.isatty():
            print(f"\033[32m{line}\033[0m")
        else:
            print(line)


def output_paths(args):
    if args.output:
        raw = args.output
    else:
        RESULT_DIR.mkdir(parents=True, exist_ok=True)
        raw = RESULT_DIR / (
            f"bop-cc-4bit-{args.arch}-cpu{args.core}-pairs{args.train_pairs}-"
            f"seed{args.seed}.txt"
        )
    summary = raw.with_name(f"{raw.stem}-summary.tsv")
    if not args.output:
        run = 2
        original = raw
        while raw.exists() or summary.exists():
            raw = original.with_name(f"{original.stem}-run{run}{original.suffix}")
            summary = raw.with_name(f"{raw.stem}-summary.tsv")
            run += 1
    return raw, summary


def save_results(args, output, parsed):
    raw, summary = output_paths(args)
    raw.parent.mkdir(parents=True, exist_ok=True)
    raw.write_text(output)
    error = parsed["error"]
    bandwidth = parsed["bandwidth"]
    lines = [
        "# BOP random offset inference cache-probe result",
        f"# arch={args.arch} core={args.core} rounds={args.rounds} "
        f"train_pairs={args.train_pairs} seed={args.seed} "
        "random_value_range=1..32",
        "# " + " ".join(f"{key}={error.get(key, 0)}" for key in (
            "correct", "errors", "error_rate", "error_rate_percent")),
        "# " + " ".join(f"{key}={bandwidth.get(key, 0)}" for key in (
            "rounds_elapsed_ns", "bits_transferred", "bandwidth_bit_s",
            "bandwidth_kbit_s")),
    ]
    summary.parent.mkdir(parents=True, exist_ok=True)
    summary.write_text("\n".join(lines) + "\n")


def main(argv=None):
    args = parse_args(argv)
    if args.describe:
        print_schedule(args)
        return 0
    binary = BIN_DIR / "bop-cc-4bit" if args.no_build else build(args)
    if args.no_run:
        return 0
    output, parsed = run_binary(args, binary)
    save_results(args, output, parsed)
    print_summary(parsed)
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (OSError, RuntimeError, subprocess.CalledProcessError) as error:
        print(f"error: {error}", file=sys.stderr)
        sys.exit(1)
