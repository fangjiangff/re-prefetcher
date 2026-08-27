#!/usr/bin/env python3

import argparse
import os
import platform
import shutil
import signal
import subprocess
import sys
from pathlib import Path


HERE = Path(__file__).resolve().parent
REPO = HERE.parent
SRC = HERE / "test-index-pc.c"
UTIL_SRC = REPO / "re0-stride-prefetcher" / "until.c"
BIN_DIR = HERE / "bin"
RESULT_DIR = HERE / "res"
PMU_MODULE_NAME = "prefetch_pmu_event"
PMU_MODULE_DIR = REPO / "kernel-modules" / "prefetch-pmu-event"


def default_compiler():
    if os.environ.get("CC"):
        return os.environ["CC"]
    if platform.machine().lower() in ("aarch64", "arm64"):
        return "gcc"
    if shutil.which("aarch64-linux-gnu-gcc"):
        return "aarch64-linux-gnu-gcc"
    return "gcc"


def parse_args():
    parser = argparse.ArgumentParser(
        description=(
            "Test whether Cortex-A725 CMC training and triggering require "
            "a stable load PC."
        )
    )
    parser.add_argument("--arch", choices=["A725"], default="A725")
    parser.add_argument("--core", "--cpu", dest="core", type=int, default=4)
    parser.add_argument("--rounds", type=int, default=500)
    parser.add_argument("--threshold-ns", type=int, default=96)
    parser.add_argument("--training-replays", type=int, default=8)
    parser.add_argument("--wait-nops", type=int, default=1000)
    parser.add_argument(
        "--pmu", action=argparse.BooleanOptionalAction, default=True,
        help="Measure A725 L1/L2 hardware-prefetch PMU events.",
    )
    parser.add_argument(
        "--cache-probe", action=argparse.BooleanOptionalAction, default=True,
        help="Probe node1 after triggering node0.",
    )
    parser.add_argument("--cc", "--compiler", dest="cc",
                        default=default_compiler())
    parser.add_argument("--output")
    parser.add_argument("--no-build", action="store_true")
    parser.add_argument("--no-run", "--build-only", dest="no_run",
                        action="store_true")
    args = parser.parse_args()

    if args.core < 0 or args.rounds <= 0 or args.threshold_ns <= 0:
        parser.error("core must be non-negative; rounds and threshold must be positive")
    if args.training_replays <= 0 or args.wait_nops < 0:
        parser.error("training-replays must be positive; wait-nops non-negative")
    if not args.pmu and not args.cache_probe:
        parser.error("enable at least one of --pmu and --cache-probe")
    return args


def binary_path():
    return BIN_DIR / "test-index-pc-A725"


def build(args):
    output = binary_path()
    BIN_DIR.mkdir(parents=True, exist_ok=True)
    command = [
        args.cc,
        "-x", "c",
        "-std=gnu11",
        "-O2",
        "-Wall",
        "-Wextra",
        "-march=armv8.5-a+predres",
        "-DENABLE_CPP_RCTX=1",
        f"-DENABLE_PMU={int(args.pmu)}",
        f"-DENABLE_CACHE_PROBE={int(args.cache_probe)}",
        str(SRC),
        str(UTIL_SRC),
        "-o", str(output),
    ]
    print("# build:", " ".join(str(item) for item in command))
    subprocess.run(command, check=True)
    return output


def module_script(name, *arguments):
    command = ["sudo", str(PMU_MODULE_DIR / name),
               *(str(value) for value in arguments)]
    print("# module:", " ".join(command))
    completed = subprocess.run(command, check=False)
    if completed.returncode != 0:
        raise RuntimeError(
            f"{' '.join(command)} exited with {completed.returncode}"
        )


def load_pmu_module(core):
    module_dir = Path("/sys/module") / PMU_MODULE_NAME
    if module_dir.is_dir():
        raise RuntimeError(
            f"{PMU_MODULE_NAME} is already loaded; refusing to replace it"
        )
    module_script("module_load.sh", "--cpu", core)
    try:
        target_cpu = int(
            (module_dir / "parameters" / "target_cpu").read_text().strip(), 0
        )
        if target_cpu != core:
            raise RuntimeError(
                f"PMU module targets CPU{target_cpu}, expected CPU{core}"
            )
    except Exception:
        module_script("module_unload.sh")
        raise
    print(f"# direct PMU module targets A725 CPU{core}")


def unload_pmu_module():
    module_script("module_unload.sh")


def run_binary(args, binary):
    if platform.machine().lower() not in ("aarch64", "arm64"):
        raise RuntimeError("execution requires an AArch64 host; use --no-run")
    command = [
        "taskset", "-c", str(args.core), str(binary),
        str(args.rounds), str(args.threshold_ns),
        str(args.training_replays), str(args.wait_nops),
    ]
    print("# run:", " ".join(command))
    return subprocess.run(
        command, check=False, text=True,
        stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
    )


def failure_status(completed):
    if completed.returncode == 0:
        return None
    if completed.returncode < 0:
        try:
            return signal.Signals(-completed.returncode).name
        except ValueError:
            return f"signal {-completed.returncode}"
    return f"status {completed.returncode}"


def parse_rows(text):
    rows = []
    header_seen = False
    for raw_line in text.splitlines():
        line = raw_line.strip()
        if line.startswith("case\tpc_mode"):
            header_seen = True
            continue
        if not header_seen or not line or line.startswith("#"):
            continue
        fields = line.split()
        if len(fields) != 12:
            continue
        try:
            rows.append({
                "case": fields[0],
                "pc_mode": int(fields[1]),
                "distinct_train_pcs": int(fields[2]),
                "training_replays": int(fields[3]),
                "samples": int(fields[4]),
                "hits": int(fields[5]),
                "hit_per_1000": float(fields[6]),
                "avg_latency_ns": float(fields[7]),
                "l1_events": int(fields[8]),
                "avg_l1_pmu": float(fields[9]),
                "l2_events": int(fields[10]),
                "avg_l2_pmu": float(fields[11]),
            })
        except ValueError:
            continue
    return rows


def green(message):
    if sys.stdout.isatty():
        return f"\033[32m{message}\033[0m"
    return message


def print_summary(text):
    rows = parse_rows(text)
    by_case = {row["case"]: row for row in rows}
    baseline = by_case.get("untrained")
    if baseline is None:
        return
    for row in rows:
        if row["case"] == "untrained":
            continue
        print(green(
            f"# delta {row['case']}: "
            f"cache_hits_per_1000="
            f"{row['hit_per_1000'] - baseline['hit_per_1000']:+.2f}, "
            f"avg_latency_ns="
            f"{row['avg_latency_ns'] - baseline['avg_latency_ns']:+.2f}, "
            f"avg_l2_pmu="
            f"{row['avg_l2_pmu'] - baseline['avg_l2_pmu']:+.4f}"
        ))


def output_path(args):
    if args.output:
        path = Path(args.output)
    else:
        RESULT_DIR.mkdir(parents=True, exist_ok=True)
        path = RESULT_DIR / (
            f"index-pc-A725-cpu{args.core}-r{args.training_replays}.txt"
        )
    path.parent.mkdir(parents=True, exist_ok=True)
    return path


def main():
    args = parse_args()
    binary = binary_path() if args.no_build else build(args)
    if args.no_run:
        print(f"# binary: {binary}")
        return 0
    if not binary.exists():
        raise SystemExit(f"binary not found: {binary}")

    pmu_loaded = False
    try:
        if args.pmu:
            load_pmu_module(args.core)
            pmu_loaded = True
        completed = run_binary(args, binary)
        status = failure_status(completed)
        if status is not None:
            print(f"error: run failed with {status}", file=sys.stderr)
            if completed.stdout:
                print(completed.stdout, file=sys.stderr, end="")
            return (128 - completed.returncode if completed.returncode < 0
                    else completed.returncode)

        print(completed.stdout, end="" if completed.stdout.endswith("\n") else "\n")
        path = output_path(args)
        path.write_text(completed.stdout)
        print(f"# output: {path}")
        print_summary(completed.stdout)
        return 0
    finally:
        if pmu_loaded:
            try:
                unload_pmu_module()
            except RuntimeError as error:
                print(f"error: PMU module unload failed: {error}",
                      file=sys.stderr)


if __name__ == "__main__":
    raise SystemExit(main())
