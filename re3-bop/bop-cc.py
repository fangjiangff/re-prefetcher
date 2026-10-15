#!/usr/bin/env python3

import argparse
import importlib.util
import platform
import signal
import subprocess
import sys
from pathlib import Path


HERE = Path(__file__).resolve().parent
REPO = HERE.parent
SRC = HERE / "bop-cc.c"
UTIL_SRC = REPO / "re0-stride-prefetcher" / "until.c"
BIN_DIR = REPO / "bin"
RESULT_DIR = HERE / "res"
ARCH_CONFIG = {"X925": {"core": 6, "threshold_ns": 80},
               "A725": {"core": 4, "threshold_ns": 100}}


def load_test_bop_helpers():
    path = HERE / "test-bop.py"
    spec = importlib.util.spec_from_file_location("test_bop_helpers", path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def default_compiler():
    return load_test_bop_helpers().default_compiler()


def parse_args(argv=None):
    parser = argparse.ArgumentParser(
        description=(
            "Run bop-cc: randomly train or skip BOP each round, then "
            "count prefetch events with one trigger using PMU."
        )
    )
    parser.add_argument("--arch", choices=ARCH_CONFIG, default="X925")
    parser.add_argument("--core", "--cpu", dest="core", type=int)
    parser.add_argument("--rounds", type=int, default=1000)
    parser.add_argument("--threshold-ns", type=int, default=None,
                        help="retained for CLI compatibility; PMU mode does not use it")
    parser.add_argument("--train-pairs", type=int, default=650)
    parser.add_argument("--offset", "--offset-lines", dest="offset_lines",
                        type=int, default=16)
    parser.add_argument("--seed", type=lambda value: int(value, 0), default=0x243F6A8885A308D3)
    parser.add_argument("--wait-nops", type=int, default=2000)
    parser.add_argument("--cc", "--compiler", dest="cc", default=None)
    parser.add_argument("--output", type=Path)
    parser.add_argument("--no-module", action="store_true",
                        help=("skip Python-side PMU module management; the "
                              "C program still requires a working PMU"))
    parser.add_argument("--describe", action="store_true")
    parser.add_argument("--no-build", action="store_true")
    parser.add_argument("--no-run", "--build-only", dest="no_run", action="store_true")
    args = parser.parse_args(argv)
    config = ARCH_CONFIG[args.arch]
    if args.core is None:
        args.core = config["core"]
    if args.threshold_ns is None:
        args.threshold_ns = config["threshold_ns"]
    if args.cc is None:
        args.cc = default_compiler()
    if args.core < 0 or args.wait_nops < 0:
        parser.error("core and wait-nops must be non-negative")
    if args.rounds <= 0 or args.train_pairs <= 0:
        parser.error("rounds and train-pairs must be positive")
    if args.offset_lines <= 0 or args.offset_lines >= 64:
        parser.error("offset must be in cache lines 1..63")
    if args.seed < 0 or args.seed >= 1 << 64:
        parser.error("seed must fit in unsigned 64 bits")
    pages = args.train_pairs + (args.train_pairs + 6) // 7 + 1
    if pages > 8192:
        parser.error("training pages exceed the 8192-page pool")
    return args


def print_schedule(args):
    print(f"# arch={args.arch} core={args.core} measurement=pmu "
          f"trigger_batch=1 train_pairs={args.train_pairs} "
          f"offset_lines={args.offset_lines} seed={args.seed}")
    print("# each round chooses trained=0 or trained=1 from SplitMix64")


def binary_path():
    return BIN_DIR / "bop-cc"


def build(args):
    output = binary_path()
    BIN_DIR.mkdir(parents=True, exist_ok=True)
    command = [
        args.cc, "-std=gnu11", "-O2", "-Wall", "-Wextra", "-Werror",
        "-Wno-unused-result", "-march=armv8.5-a+predres",
        "-DENABLE_CPP_RCTX=1", "-DENABLE_PMU=1", "-DENABLE_CACHE_PROBE=0",
        f"-DPREFETCH_WAIT_NOPS={args.wait_nops}", str(SRC), str(UTIL_SRC),
        "-o", str(output),
    ]
    subprocess.run(command, check=True)
    return output


def parse_output(text):
    rounds = []
    conditions = {}
    elapsed_ns = None
    bandwidth = {}
    error = {}
    for line in text.splitlines():
        fields = line.split()
        if fields and fields[0] == "#":
            fields = fields[1:]
        if fields and fields[0].startswith("rounds_elapsed_ns="):
            elapsed_ns = int(fields[0].split("=", 1)[1])
            continue
        if fields and fields[0].startswith("bits_transferred="):
            values = dict(item.split("=", 1) for item in fields)
            bandwidth = {
                key: float(value) for key, value in values.items()
            }
            continue
        if fields and fields[0].startswith("bit_decisions="):
            values = dict(item.split("=", 1) for item in fields)
            error = {key: float(value) for key, value in values.items()}
            continue
        if fields and fields[0].startswith("round="):
            values = dict(item.split("=", 1) for item in fields)
            rounds.append({key: int(value) for key, value in values.items()})
        elif fields and fields[0].startswith("condition="):
            values = dict(item.split("=", 1) for item in fields)
            name = values.pop("condition")
            conditions[name] = {
                key: int(value) for key, value in values.items()
            }
    if not conditions:
        raise RuntimeError("bop-cc output has no condition summary")
    return {"rounds": rounds, "conditions": conditions,
            "elapsed_ns": elapsed_ns, "bandwidth": bandwidth,
            "error": error}


def run_binary(args, binary):
    if platform.machine().lower() not in ("aarch64", "arm64"):
        raise RuntimeError("execution requires an AArch64 host; use --no-run")
    command = [
        "taskset", "-c", str(args.core), str(binary), str(args.rounds),
        str(args.threshold_ns), str(args.train_pairs), "1",
        str(args.offset_lines), str(args.seed),
    ]
    completed = subprocess.run(command, text=True,
                               stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    if completed.returncode != 0:
        reason = (signal.Signals(-completed.returncode).name
                  if completed.returncode < 0 else f"status {completed.returncode}")
        raise RuntimeError(f"bop-cc failed with {reason}:\n{completed.stdout}")
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
            f"bop-cc-{args.arch}-cpu{args.core}-pairs{args.train_pairs}-"
            f"off{args.offset_lines}-seed{args.seed}.txt"
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
    rows = [
        "# BOP conditional-training PMU result",
        f"# arch={args.arch} core={args.core} rounds={args.rounds} "
        f"train_pairs={args.train_pairs} offset_lines={args.offset_lines} "
        f"seed={args.seed} trigger_batch=1",
        f"rounds_elapsed_ns={parsed['elapsed_ns'] or 0}",
        "# " + " ".join(
            f"{key}={parsed['bandwidth'].get(key, 0.0)}"
            for key in ("bits_transferred", "bandwidth_bit_s",
                        "bandwidth_kbit_s")
        ),
        "# " + " ".join(
            f"{key}={parsed['error'].get(key, 0.0)}"
            for key in ("bit_decisions", "correct", "errors",
                        "error_rate", "error_rate_percent")
        ),
        "condition\trounds\tl1_prefetches\tl2_prefetches\ttotal_prefetches",
    ]
    for condition in ("untrained", "trained"):
        values = parsed["conditions"].get(condition, {})
        rows.append("\t".join([
            condition,
            *(str(values.get(key, 0)) for key in (
                "rounds", "l1_prefetches", "l2_prefetches",
                "total_prefetches")),
        ]))
    summary.parent.mkdir(parents=True, exist_ok=True)
    summary.write_text("\n".join(rows) + "\n")


def main(argv=None):
    args = parse_args(argv)
    if args.describe:
        print_schedule(args)
        return 0
    binary = binary_path() if args.no_build else build(args)
    if args.no_run:
        return 0
    helpers = load_test_bop_helpers()
    module_loaded = False
    try:
        if not args.no_module:
            module_loaded = helpers.load_pmu_module(args)
        output, parsed = run_binary(args, binary)
        save_results(args, output, parsed)
        print_summary(parsed)
    finally:
        if module_loaded:
            helpers.unload_pmu_module()
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (OSError, RuntimeError, subprocess.CalledProcessError) as error:
        print(f"error: {error}", file=sys.stderr)
        sys.exit(1)
