#!/usr/bin/env python3

import argparse
import platform
import shutil
import signal
import subprocess
import sys
from pathlib import Path


HERE = Path(__file__).resolve().parent
REPO = HERE.parent
SRC = HERE / "test-bop-retention.c"
UTIL_SRC = REPO / "re0-stride-prefetcher" / "until.c"
BIN_DIR = REPO / "bin"
RESULT_DIR = HERE / "res"
PMU_MODULE_NAME = "prefetch_pmu_event"
PMU_MODULE_DIR = REPO / "kernel-modules" / "prefetch-pmu-event"
DEFAULT_DECAY_VALUES = (
    0, 64, 128, 256, 512, 1024, 2048, 3072, 4096,
    4608, 5120, 6144, 8192, 12288, 16384,
)
ARCH_CONFIG = {
    "X925": {"core": 6, "threshold_ns": 80},
    "A725": {"core": 4, "threshold_ns": 120},
}


def parse_decay_values(text):
    try:
        values = tuple(int(item, 0) for item in text.split(","))
    except ValueError as error:
        raise argparse.ArgumentTypeError("decay values must be integers") from error
    if not values or any(value < 0 for value in values):
        raise argparse.ArgumentTypeError("decay values must be non-negative")
    if len(set(values)) != len(values):
        raise argparse.ArgumentTypeError("decay values must be unique")
    return values


def default_compiler():
    if platform.machine().lower() in ("aarch64", "arm64"):
        return "gcc"
    return shutil.which("aarch64-linux-gnu-gcc") or "gcc"


def parse_args():
    parser = argparse.ArgumentParser(
        description="Measure how many unrelated triggers retain a learned BOP offset."
    )
    parser.add_argument("--arch", choices=ARCH_CONFIG, default="X925")
    parser.add_argument("--core", "--cpu", dest="core", type=int)
    parser.add_argument("--threshold-ns", type=int)
    parser.add_argument("--offset-lines", type=int, default=16)
    parser.add_argument("--train-pairs", type=int, default=3000)
    parser.add_argument("--repeats", type=int, default=10)
    parser.add_argument("--probe-batch", type=int, default=64)
    parser.add_argument(
        "--decay-values",
        type=parse_decay_values,
        default=DEFAULT_DECAY_VALUES,
        help="comma-separated unrelated-trigger counts",
    )
    parser.add_argument(
        "--stage", choices=("both", "pmu", "cache"), default="both"
    )
    parser.add_argument("--wait-nops", type=int, default=2000)
    parser.add_argument("--cc", default=default_compiler())
    parser.add_argument("--output", type=Path)
    parser.add_argument("--describe", action="store_true")
    parser.add_argument("--no-build", action="store_true")
    parser.add_argument("--no-run", action="store_true")
    args = parser.parse_args()

    config = ARCH_CONFIG[args.arch]
    if args.core is None:
        args.core = config["core"]
    if args.threshold_ns is None:
        args.threshold_ns = config["threshold_ns"]
    if args.describe:
        return args
    if args.core < 0 or args.wait_nops < 0:
        parser.error("core and wait-nops must be non-negative")
    if args.threshold_ns <= 0 or args.train_pairs <= 0 or args.repeats <= 0:
        parser.error("threshold-ns, train-pairs, and repeats must be positive")
    if args.probe_batch <= 0:
        parser.error("probe-batch must be positive")
    if not 1 <= args.offset_lines <= 63:
        parser.error("offset-lines must be in 1..63")
    if max(args.decay_values) + args.train_pairs + (args.train_pairs + 6) // 7 + args.probe_batch > 32768:
        parser.error("address sets exceed the C program's 32768-page pool")
    return args


def requested_stages(args):
    if args.stage == "pmu":
        return ("pmu",)
    if args.stage == "cache":
        return ("cache",)
    return ("pmu", "cache")


def print_schedule(args):
    print("stage\tdecay_triggers")
    for stage in requested_stages(args):
        for decay in args.decay_values:
            print(f"{stage}\t{decay}")


def binary_path(stage):
    return BIN_DIR / f"test-bop-retention-{stage}"


def build(args, stage):
    output = binary_path(stage)
    BIN_DIR.mkdir(parents=True, exist_ok=True)
    command = [
        args.cc,
        "-std=gnu11", "-O2", "-Wall", "-Wextra", "-Werror",
        "-Wno-unused-result", "-march=armv8.5-a+predres",
        "-DENABLE_CPP_RCTX=1",
        f"-DENABLE_PMU={int(stage == 'pmu')}",
        f"-DENABLE_CACHE_PROBE={int(stage == 'cache')}",
        f"-DPREFETCH_WAIT_NOPS={args.wait_nops}",
        str(SRC), str(UTIL_SRC), "-o", str(output),
    ]
    print(f"# build {stage}:", " ".join(command))
    subprocess.run(command, check=True)
    return output


def run_binary(args, binary, decay):
    if platform.machine().lower() not in ("aarch64", "arm64"):
        raise RuntimeError("execution requires an AArch64 host; use --no-run")
    command = [
        str(binary),
        "--cpu", str(args.core),
        "--repeats", str(args.repeats),
        "--threshold-ns", str(args.threshold_ns),
        "--train-pairs", str(args.train_pairs),
        "--decay-triggers", str(decay),
        "--probe-batch", str(args.probe_batch),
        "--offset-lines", str(args.offset_lines),
    ]
    print("# run:", " ".join(command))
    completed = subprocess.run(
        command, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT
    )
    if completed.returncode != 0:
        reason = (
            signal.Signals(-completed.returncode).name
            if completed.returncode < 0
            else f"status {completed.returncode}"
        )
        raise RuntimeError(f"retention run failed with {reason}:\n{completed.stdout}")
    return parse_result(completed.stdout)


def parse_result(output):
    lines = output.splitlines()
    header = next((line for line in lines if line.startswith("mode\t")), None)
    if header is None:
        raise RuntimeError(f"retention output has no table header:\n{output}")
    index = lines.index(header)
    if index + 1 == len(lines):
        raise RuntimeError("retention output has no data row")
    names = header.split("\t")
    values = lines[index + 1].split("\t")
    if len(names) != len(values):
        raise RuntimeError("retention output row has the wrong column count")
    return dict(zip(names, values))


def module_script(name, *arguments):
    command = ["sudo", str(PMU_MODULE_DIR / name), *(str(x) for x in arguments)]
    print("# module:", " ".join(command))
    subprocess.run(command, check=True)


def validate_pmu_module(args):
    module_dir = Path("/sys/module") / PMU_MODULE_NAME
    target = int((module_dir / "parameters" / "target_cpu").read_text().strip(), 0)
    if target != args.core:
        raise RuntimeError(f"PMU module targets CPU{target}, expected CPU{args.core}")
    perf_access = Path("/proc/sys/kernel/perf_user_access")
    if perf_access.is_file() and perf_access.read_text().strip() != "1":
        raise RuntimeError("kernel.perf_user_access must be 1")


def load_pmu_module(args):
    module_dir = Path("/sys/module") / PMU_MODULE_NAME
    if module_dir.is_dir():
        validate_pmu_module(args)
        return False
    module_script("module_load.sh", "--cpu", args.core)
    try:
        validate_pmu_module(args)
    except Exception:
        module_script("module_unload.sh")
        raise
    return True


def output_path(args):
    if args.output:
        return args.output
    RESULT_DIR.mkdir(parents=True, exist_ok=True)
    base = RESULT_DIR / (
        f"bop-{args.arch}-cpu{args.core}-off{args.offset_lines}-"
        f"train{args.train_pairs}-repeats{args.repeats}-"
        f"probe{args.probe_batch}-retention.tsv"
    )
    if not base.exists():
        return base
    run = 2
    while True:
        candidate = base.with_name(f"{base.stem}-run{run}{base.suffix}")
        if not candidate.exists():
            return candidate
        run += 1


def save_results(args, results):
    columns = [
        "decay_triggers", "pmu_l1_per_trigger", "pmu_l2_per_trigger",
        "cache_hits_per_1000", "cache_avg_latency_ns",
    ]
    by_decay = {decay: {} for decay in args.decay_values}
    for (stage, decay), row in results.items():
        by_decay[decay][stage] = row
    stages = ",".join(requested_stages(args))
    lines = [
        "# BOP best-offset retention: PMU pass precedes cache pass",
        f"# arch={args.arch} core={args.core} "
        f"offset_lines={args.offset_lines} train_pairs={args.train_pairs} "
        f"repeats={args.repeats} probe_batch={args.probe_batch} "
        f"threshold_ns={args.threshold_ns} wait_nops={args.wait_nops} "
        f"stages={stages}",
        "# decay_targets_demand_accessed=no",
        "\t".join(columns),
    ]
    for decay in args.decay_values:
        pmu = by_decay[decay].get("pmu", {})
        cache = by_decay[decay].get("cache", {})
        lines.append("\t".join([
            str(decay),
            pmu.get("l1_per_trigger", "-"),
            pmu.get("l2_per_trigger", "-"),
            cache.get("hits_per_1000", "-"),
            cache.get("avg_latency_ns", "-"),
        ]))
    path = output_path(args)
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text("\n".join(lines) + "\n")
    print(f"# output: {path}")


def main():
    args = parse_args()
    if args.describe:
        print_schedule(args)
        return 0

    stages = requested_stages(args)
    binaries = {
        stage: binary_path(stage) if args.no_build else build(args, stage)
        for stage in stages
    }
    if args.no_run:
        return 0

    results = {}
    loaded_here = False
    try:
        for stage in stages:
            if stage == "pmu":
                loaded_here = load_pmu_module(args)
            for decay in args.decay_values:
                results[(stage, decay)] = run_binary(args, binaries[stage], decay)
            if stage == "pmu" and loaded_here:
                module_script("module_unload.sh")
                loaded_here = False
    finally:
        if loaded_here:
            module_script("module_unload.sh")
    save_results(args, results)
    return 0


if __name__ == "__main__":
    sys.exit(main())
