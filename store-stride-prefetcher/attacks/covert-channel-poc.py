import argparse
import os
import subprocess
import sys

ROOT_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
if ROOT_DIR not in sys.path:
    sys.path.insert(0, ROOT_DIR)

from cross_test_config import ARCH_CONFIG


BASE_DIR = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.join(BASE_DIR, "covert-channel-poc.c")
UTIL_SRC = os.path.join(ROOT_DIR, "until.c")
OUT = os.path.join(ROOT_DIR, "bin", "covert-channel-poc")

DEFAULT_STRIDE_LINES = 5
DEFAULT_TRAIN_STEP = 3
DEFAULT_ROUNDS = 10000
GREEN = "\033[32m"
RESET = "\033[0m"


def parse_args():
    parser = argparse.ArgumentParser(
        description="Run the Cortex-X925 PMU covert-channel experiment."
    )
    parser.add_argument(
        "--arch",
        choices=["X925"],
        default="X925",
        help="Target architecture. Default: X925",
    )
    parser.add_argument(
        "--core",
        type=int,
        default=None,
        help="CPU core used by taskset. Default: configured X925 core",
    )
    parser.add_argument(
        "--stride",
        type=int,
        default=DEFAULT_STRIDE_LINES,
        help=f"Stride in cache lines. Default: {DEFAULT_STRIDE_LINES}",
    )
    parser.add_argument(
        "--train-step",
        type=int,
        default=DEFAULT_TRAIN_STEP,
        help=f"Number of training accesses. Default: {DEFAULT_TRAIN_STEP}",
    )
    parser.add_argument(
        "--rounds",
        type=int,
        default=DEFAULT_ROUNDS,
        help=f"Number of transmitted bits. Default: {DEFAULT_ROUNDS}",
    )
    parser.add_argument(
        "--access",
        choices=["store", "load", "prefetch"],
        default="store",
        help="Stride access instruction. Default: store",
    )
    parser.add_argument(
        "--pmu-device",
        default=None,
        help="Override the PMU_DEVICE environment variable.",
    )
    args = parser.parse_args()

    if args.core is not None and args.core < 0:
        parser.error("--core must be >= 0")
    if args.stride < 1:
        parser.error("--stride must be >= 1")
    if args.train_step < 1:
        parser.error("--train-step must be >= 1")
    if args.rounds < 1:
        parser.error("--rounds must be >= 1")

    return args


def compile_test(args):
    os.makedirs(os.path.dirname(OUT), exist_ok=True)
    command = [
        os.environ.get("CC", "gcc"),
        "-march=armv8.5-a+predres",
        "-std=gnu11",
        "-O0",
        "-static",
        f"-DSTRIDE_BYTES={args.stride * 64}",
        f"-DTRAIN_STEP={args.train_step}",
        f"-DROUNDS={args.rounds}",
        f"-DTRAIN_ACCESS_LOAD={1 if args.access == 'load' else 0}",
        f"-DTRAIN_ACCESS_PREFETCH={1 if args.access == 'prefetch' else 0}",
        "-o",
        OUT,
        SRC,
        UTIL_SRC,
    ]
    return subprocess.run(command).returncode


def run_binary(args, core):
    env = os.environ.copy()
    if args.pmu_device is not None:
        env["PMU_DEVICE"] = args.pmu_device

    return subprocess.run(
        ["taskset", "-c", str(core), OUT],
        capture_output=True,
        text=True,
        env=env,
    )


def print_output(output):
    for line in output.splitlines():
        if not line.strip():
            continue

        normalized = line.lstrip().lstrip("#").lstrip().lower()
        if normalized.startswith(("accuracy=", "bandwidth=")):
            print(f"{GREEN}{line}{RESET}")
        else:
            print(line)


def main():
    args = parse_args()
    core = args.core if args.core is not None else ARCH_CONFIG[args.arch]["core"]

    print("=" * 60)
    print(
        f"access={args.access}, arch={args.arch}, core={core}, "
        f"stride={args.stride} lines, train_step={args.train_step}, "
        f"rounds={args.rounds}"
    )

    if compile_test(args) != 0:
        print("Compile failed", file=sys.stderr)
        return 1

    result = run_binary(args, core)
    print_output(result.stdout)
    print_output(result.stderr)

    if result.returncode != 0:
        print("Execution failed", file=sys.stderr)
        return result.returncode

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
