#!/usr/bin/env python3
"""Build and run the DMP/AoP existence test on an AArch64 core."""

import argparse
import os
import platform
import re
import shutil
import signal
import subprocess
import sys
from pathlib import Path


HERE = Path(__file__).resolve().parent
REPO = HERE.parent
SRC = HERE / "test-exist.c"
UTIL_SRC = REPO / "re0-stride-prefetcher" / "until.c"
BIN_DIR = HERE / "bin"
RESULT_DIR = HERE / "res"
PMU_MODULE_NAME = "prefetch_pmu_event"
PMU_MODULE_DIR = REPO / "kernel-modules" / "prefetch-pmu-event"

ARCH_CONFIG = {
    "X925": {"core": 6, "threshold_ns": 80},
    "A725": {"core": 4, "threshold_ns": 100},
}
CASE_ORDER = [
    "idle_no_activation",
    "idle_ahead2_no_activation",
    "nonpointer_control",
    "nonpointer_shuffled_targets",
    "pointer_content_no_deref",
    "pointer_array_loads_only",
    "pointer_array_one_dereference",
    "pointer_array_two_dereferences",
    "pointer_array_first_deref_nextline",
    "pointer_array_second_deref_nextline",
    "pointer_array_ahead2",
    "pointer_array_ahead3",
    "pointer_array_ahead4",
    "pointer_array_ahead5",
    "pointer_array_ahead6",
    "pointer_array_loads_only_ahead2",
    "pointer_array_one_dereference_ahead2",
]

PMU_RE = re.compile(r"^# PMU\s+(\S+)\s+per_sample=([0-9.]+)$")


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
            "Build and run the DMP/AoP existence test. It compares a "
            "non-pointer cache-line payload with valid-pointer payloads."
        )
    )
    parser.add_argument("--arch", choices=ARCH_CONFIG, default="A725")
    parser.add_argument("--core", "--cpu", dest="core", type=int)
    parser.add_argument("--rounds", type=int, default=4096)
    parser.add_argument("--threshold-ns", type=int)
    parser.add_argument("--wait-nops", type=int, default=1000)
    parser.add_argument(
        "--pmu", action=argparse.BooleanOptionalAction, default=True,
        help="Build/run ENABLE_PMU=1, ENABLE_CACHE_PROBE=0.",
    )
    parser.add_argument(
        "--cache-probe", action=argparse.BooleanOptionalAction, default=True,
        help="Build/run ENABLE_PMU=0, ENABLE_CACHE_PROBE=1.",
    )
    parser.add_argument("--cc", "--compiler", dest="cc", default=default_compiler())
    parser.add_argument("--output", help="Combined TSV output path.")
    parser.add_argument("--plot", help="PNG output path.")
    parser.add_argument("--no-plot", action="store_true")
    parser.add_argument("--no-build", action="store_true")
    parser.add_argument("--no-run", "--build-only", dest="no_run",
                        action="store_true")
    args = parser.parse_args()

    config = ARCH_CONFIG[args.arch]
    if args.core is None:
        args.core = config["core"]
    if args.threshold_ns is None:
        args.threshold_ns = config["threshold_ns"]
    if args.rounds <= 0 or args.threshold_ns <= 0:
        parser.error("rounds and threshold-ns must be positive")
    if args.core < 0 or args.wait_nops < 0:
        parser.error("core and wait-nops must be non-negative")
    if not args.pmu and not args.cache_probe:
        parser.error("enable at least one of --pmu and --cache-probe")
    return args


def binary_path(kind):
    return BIN_DIR / f"test-dmp-exist-{kind}"


def build(args, kind, enable_pmu, enable_cache_probe):
    BIN_DIR.mkdir(parents=True, exist_ok=True)
    output = binary_path(kind)
    command = [
        args.cc,
        "-x", "c",
        "-std=gnu11",
        "-O2",
        "-Wall",
        "-Wextra",
        "-march=armv8.5-a+predres",
        "-DENABLE_CPP_RCTX=1",
        f"-DENABLE_PMU={int(enable_pmu)}",
        f"-DENABLE_CACHE_PROBE={int(enable_cache_probe)}",
        f"-DPREFETCH_WAIT_NOPS={args.wait_nops}",
        f"-DPMU_CORE_X925={int(args.arch == 'X925')}",
        str(SRC),
        str(UTIL_SRC),
        "-o", str(output),
    ]
    print("# build", kind + ":", " ".join(command))
    subprocess.run(command, check=True)
    return output


def run_binary(args, binary, kind):
    if platform.machine().lower() not in ("aarch64", "arm64"):
        raise RuntimeError("execution requires an AArch64 host; use --no-run")
    command = [
        "taskset", "-c", str(args.core), str(binary), str(args.rounds),
        str(args.threshold_ns),
    ]
    print("# run", kind + ":", " ".join(command))
    completed = subprocess.run(
        command, check=False, text=True, stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
    )
    return completed.returncode, completed.stdout


def report_failure(kind, returncode, output):
    if returncode == 0:
        return False
    if returncode < 0:
        try:
            reason = signal.Signals(-returncode).name
        except ValueError:
            reason = f"signal {-returncode}"
    else:
        reason = f"status {returncode}"
    print(f"error: {kind} run failed with {reason}", file=sys.stderr)
    if output:
        print(output, end="" if output.endswith("\n") else "\n", file=sys.stderr)
    return True


def module_script(name, *arguments):
    command = ["sudo", str(PMU_MODULE_DIR / name),
               *(str(value) for value in arguments)]
    print("# module:", " ".join(command))
    completed = subprocess.run(command, check=False)
    if completed.returncode != 0:
        raise RuntimeError(f"{' '.join(command)} exited with {completed.returncode}")


def load_pmu_module(args):
    module_dir = Path("/sys/module") / PMU_MODULE_NAME
    if module_dir.is_dir():
        raise RuntimeError(f"{PMU_MODULE_NAME} is already loaded; refusing to replace it")
    module_script("module_load.sh", "--cpu", args.core)
    try:
        target_path = module_dir / "parameters" / "target_cpu"
        target_cpu = int(target_path.read_text().strip(), 0)
        if target_cpu != args.core:
            raise RuntimeError(
                f"PMU module targets CPU{target_cpu}, expected CPU{args.core}"
            )
        perf_access = Path("/proc/sys/kernel/perf_user_access")
        if perf_access.is_file() and perf_access.read_text().strip() != "1":
            raise RuntimeError("kernel.perf_user_access must be 1")
    except Exception:
        module_script("module_unload.sh")
        raise
    print(f"# direct PMU module targets CPU{args.core}")


def unload_pmu_module():
    module_script("module_unload.sh")


def parse_output(text):
    section = None
    cases = {}
    for raw_line in text.splitlines():
        line = raw_line.strip()
        if line.startswith("[") and line.endswith("]"):
            section = line[1:-1]
            cases.setdefault(section, {"slots": {}})
            continue
        if section is None:
            continue
        match = PMU_RE.match(line)
        if match:
            cases[section][match.group(1)] = float(match.group(2))
            continue
        if not line or line.startswith("#"):
            continue
        fields = line.split()
        if len(fields) not in (5, 7):
            continue
        try:
            slot = int(fields[0])
            cases[section]["slots"][slot] = {
                "probes": int(fields[1]),
                "hits": int(fields[2]),
                "hits_per_1000": int(fields[3]),
                "avg_latency_ns": float(fields[4]),
                "avg_l1_pmu": float(fields[5]) if len(fields) == 7 else None,
                "avg_l2_pmu": float(fields[6]) if len(fields) == 7 else None,
            }
        except ValueError:
            continue
    return cases


def aggregate_slots(case):
    slots = case.get("slots", {}).values()
    probes = sum(slot["probes"] for slot in slots)
    hits = sum(slot["hits"] for slot in slots)
    latency_weighted = sum(
        slot["avg_latency_ns"] * slot["probes"] for slot in slots
    )
    return {
        "probes": probes,
        "hits": hits,
        "hits_per_1000": 0 if probes == 0 else hits * 1000 // probes,
        "avg_latency_ns": 0.0 if probes == 0 else latency_weighted / probes,
    }


def output_paths(args):
    if args.output:
        combined = Path(args.output)
    else:
        RESULT_DIR.mkdir(parents=True, exist_ok=True)
        combined = RESULT_DIR / (
            f"dmp-exist-{args.arch}-cpu{args.core}-thr{args.threshold_ns}-"
            f"r{args.rounds}.tsv"
        )
    combined.parent.mkdir(parents=True, exist_ok=True)
    return (
        combined,
        combined.with_name(combined.stem + "-pmu.txt"),
        combined.with_name(combined.stem + "-cache.txt"),
    )


def merged_table(pmu_text, cache_text):
    pmu = parse_output(pmu_text)
    cache = parse_output(cache_text)
    lines = [
        "# DMP existence test; PMU and cache probe are separate runs",
        "case\tprobes\thits\thits_per_1000\tavg_latency_ns\t"
        "l1_pmu_per_sample\tl2_pmu_per_sample",
    ]
    for case_name in CASE_ORDER:
        cache_case = aggregate_slots(cache.get(case_name, {}))
        pmu_case = pmu.get(case_name, {})
        lines.append("\t".join(str(value) for value in [
            case_name,
            cache_case["probes"] if cache_case["probes"] else "-",
            cache_case["hits"] if cache_case["probes"] else "-",
            cache_case["hits_per_1000"] if cache_case["probes"] else "-",
            (f'{cache_case["avg_latency_ns"]:.2f}'
             if cache_case["probes"] else "-"),
            (f'{pmu_case["l1d_cache_hwprf"]:.6f}'
             if "l1d_cache_hwprf" in pmu_case else "-"),
            (f'{pmu_case["l2d_cache_hwprf"]:.6f}'
             if "l2d_cache_hwprf" in pmu_case else "-"),
        ]))
    return "\n".join(lines) + "\n"


def print_summary(pmu_text, cache_text):
    pmu = parse_output(pmu_text)
    cache = parse_output(cache_text)
    baseline = aggregate_slots(cache.get("nonpointer_control", {}))
    content = aggregate_slots(cache.get("pointer_content_no_deref", {}))
    array = aggregate_slots(cache.get("pointer_array_two_dereferences", {}))

    if baseline["probes"] and content["probes"]:
        print("# cache delta no-deref-content-control: "
              f"hits_per_1000={content['hits_per_1000'] - baseline['hits_per_1000']:+d} "
              f"avg_latency_ns={content['avg_latency_ns'] - baseline['avg_latency_ns']:+.2f}")
    if baseline["probes"] and array["probes"]:
        print("# cache delta pointer-array-control: "
              f"hits_per_1000={array['hits_per_1000'] - baseline['hits_per_1000']:+d} "
              f"avg_latency_ns={array['avg_latency_ns'] - baseline['avg_latency_ns']:+.2f}")

    for event in ("l1d_cache_hwprf", "l2d_cache_hwprf"):
        if event in pmu.get("nonpointer_control", {}) and event in pmu.get("pointer_content_no_deref", {}):
            delta = (pmu["pointer_content_no_deref"][event] -
                     pmu["nonpointer_control"][event])
            print(f"# PMU delta {event} no-deref-content-control={delta:+.6f}/sample")


def plot_results(args, pmu_text, cache_text):
    if args.no_plot:
        return
    try:
        import matplotlib
        matplotlib.use("Agg")
        import matplotlib.pyplot as plt
    except ImportError:
        print("# matplotlib unavailable; skip plot", file=sys.stderr)
        return

    pmu = parse_output(pmu_text)
    cache = parse_output(cache_text)
    cache_cases = [aggregate_slots(cache.get(case, {})) for case in CASE_ORDER]
    labels = [case.replace("_", "\n") for case in CASE_ORDER]
    positions = list(range(len(CASE_ORDER)))
    figure, axes = plt.subplots(2, 1, figsize=(10, 6), constrained_layout=True)
    axes[0].bar(positions, [case["hits_per_1000"] for case in cache_cases])
    axes[0].set_ylabel("Cache hits / 1000")
    axes[0].set_xticks(positions, labels)
    axes[0].grid(axis="y", alpha=0.25)

    width = 0.35
    axes[1].bar(
        [position - width / 2 for position in positions],
        [pmu.get(case, {}).get("l1d_cache_hwprf", 0.0) for case in CASE_ORDER],
        width=width, label="L1 HW prefetch",
    )
    axes[1].bar(
        [position + width / 2 for position in positions],
        [pmu.get(case, {}).get("l2d_cache_hwprf", 0.0) for case in CASE_ORDER],
        width=width, label="L2 prefetch",
    )
    axes[1].set_ylabel("PMU events / sample")
    axes[1].set_xticks(positions, labels)
    axes[1].grid(axis="y", alpha=0.25)
    axes[1].legend(fontsize=8)
    axes[0].set_title(f"DMP/AoP existence test: {args.arch} CPU{args.core}")
    path = Path(args.plot) if args.plot else output_paths(args)[0].with_suffix(".png")
    path.parent.mkdir(parents=True, exist_ok=True)
    figure.savefig(path, dpi=300)
    plt.close(figure)
    print(f"# plot: {path}")


def main():
    args = parse_args()
    binaries = {}
    if args.pmu:
        binaries["pmu"] = binary_path("pmu") if args.no_build else build(
            args, "pmu", enable_pmu=True, enable_cache_probe=False
        )
    if args.cache_probe:
        binaries["cache"] = binary_path("cache") if args.no_build else build(
            args, "cache", enable_pmu=False, enable_cache_probe=True
        )

    if args.no_run:
        for kind, path in binaries.items():
            print(f"# binary {kind}: {path}")
        return 0
    for path in binaries.values():
        if not path.exists():
            raise SystemExit(f"binary not found: {path}")

    outputs = {}
    pmu_loaded = False
    try:
        if args.pmu:
            load_pmu_module(args)
            pmu_loaded = True
            code, text = run_binary(args, binaries["pmu"], "PMU-only")
            if report_failure("PMU-only", code, text):
                return 128 - code if code < 0 else code
            outputs["pmu"] = text
            print(text, end="" if text.endswith("\n") else "\n")

        if args.cache_probe:
            code, text = run_binary(args, binaries["cache"], "cache-only")
            if report_failure("cache-only", code, text):
                return 128 - code if code < 0 else code
            outputs["cache"] = text
            print(text, end="" if text.endswith("\n") else "\n")
    finally:
        if pmu_loaded:
            try:
                unload_pmu_module()
            except RuntimeError as exc:
                print(f"error: PMU module unload failed: {exc}", file=sys.stderr)

    combined, pmu_path, cache_path = output_paths(args)
    if "pmu" in outputs:
        pmu_path.write_text(outputs["pmu"])
        print(f"# output PMU: {pmu_path}")
    if "cache" in outputs:
        cache_path.write_text(outputs["cache"])
        print(f"# output cache: {cache_path}")
    combined.write_text(merged_table(outputs.get("pmu", ""), outputs.get("cache", "")))
    print(f"# output merged table: {combined}")
    print_summary(outputs.get("pmu", ""), outputs.get("cache", ""))
    plot_results(args, outputs.get("pmu", ""), outputs.get("cache", ""))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
