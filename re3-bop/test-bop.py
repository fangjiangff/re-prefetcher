#!/usr/bin/env python3

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
SRC = HERE / "test-bop.c"
UTIL_SRC = REPO / "re0-stride-prefetcher" / "until.c"
BIN_DIR = REPO / "bin"
RESULT_DIR = HERE / "res"
PMU_MODULE_NAME = "prefetch_pmu_event"
PMU_MODULE_DIR = REPO / "kernel-modules" / "prefetch-pmu-event"

ARCH_CONFIG = {
    "X925": {"core": 6, "threshold_ns": 80},
    "A725": {"core": 4, "threshold_ns": 120},
}

CASE_ORDER = [
    "untrained",
    "fixed_offset_expected",
]
TRAIN_PAIR_SWEEP = tuple(range(100, 3001, 100))

PMU_RE = re.compile(
    r"^# PMU\s+(\S+)\s+total=(\d+)\s+per_trigger=([0-9.]+)$"
)
CACHE_RE = re.compile(
    r"^# CACHE\s+probes=(\d+)\s+hits=(\d+)\s+"
    r"hits_per_1000=(\d+)\s+avg_latency_ns=([0-9.]+)\s+"
    r"threshold_ns=(\d+)$"
)
META_RE = re.compile(
    r"^# train_mode=(\d+)\s+probe_delta_lines=(\d+)\s+rounds=(\d+)$"
)


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
            "Build and run the BOP existence test. The experiment compares "
            "an untrained baseline with fixed-offset training at the "
            "expected probe offset."
        )
    )
    parser.add_argument("--arch", choices=ARCH_CONFIG, default="X925")
    parser.add_argument("--core", "--cpu", dest="core", type=int)
    parser.add_argument("--rounds", type=int, default=100)
    parser.add_argument("--threshold-ns", type=int)
    parser.add_argument("--train-pairs", type=int, default=1024)
    parser.add_argument(
        "--sweep-train-pairs", action="store_true",
        help="Run train-pairs=100,200,...,3000 and generate a line plot.",
    )
    parser.add_argument("--trigger-batch", type=int, default=2)
    parser.add_argument("--wait-nops", type=int, default=2000)
    parser.add_argument(
        "--pmu", action=argparse.BooleanOptionalAction, default=True,
        help="Build/run ENABLE_PMU=1, ENABLE_CACHE_PROBE=0.",
    )
    parser.add_argument(
        "--cache-probe", action=argparse.BooleanOptionalAction, default=True,
        help="Build/run ENABLE_PMU=0, ENABLE_CACHE_PROBE=1.",
    )
    parser.add_argument("--cc", "--compiler", dest="cc",
                        default=default_compiler())
    parser.add_argument("--output")
    parser.add_argument("--plot")
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
    positive = ("rounds", "threshold_ns", "train_pairs", "trigger_batch")
    if any(getattr(args, name) <= 0 for name in positive):
        parser.error(
            "rounds, threshold-ns, train-pairs, and trigger-batch "
            "must be positive"
        )
    if args.core < 0 or args.wait_nops < 0:
        parser.error("core and wait-nops must be non-negative")
    if args.trigger_batch % 2:
        parser.error("trigger-batch must be even")
    max_train_pairs = (TRAIN_PAIR_SWEEP[-1] if args.sweep_train_pairs
                       else args.train_pairs)
    if max_train_pairs + (max_train_pairs + 6) // 7 + args.trigger_batch > 4096:
        parser.error("train-pairs plus noise and trigger pages exceeds 4096")
    if not args.pmu and not args.cache_probe:
        parser.error("enable at least one of --pmu and --cache-probe")
    return args


def binary_path(kind):
    return BIN_DIR / f"test-bop-{kind}"


def build(args, kind, enable_pmu, enable_cache_probe):
    output = binary_path(kind)
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
        f"-DENABLE_PMU={int(enable_pmu)}",
        f"-DENABLE_CACHE_PROBE={int(enable_cache_probe)}",
        f"-DPREFETCH_WAIT_NOPS={args.wait_nops}",
        f"-DPMU_CORE_X925={int(args.arch == 'X925')}",
        str(SRC),
        str(UTIL_SRC),
        "-o", str(output),
    ]
    print(f"# build {kind}:", " ".join(str(item) for item in command))
    subprocess.run(command, check=True)
    return output


def run_arguments(args):
    return [
        str(args.rounds),
        str(args.threshold_ns),
        str(args.train_pairs),
        str(args.trigger_batch),
    ]


def run_binary(args, binary, kind):
    if platform.machine().lower() not in ("aarch64", "arm64"):
        raise RuntimeError("execution requires an AArch64 host; use --no-run")
    command = [
        "taskset", "-c", str(args.core), str(binary), *run_arguments(args)
    ]
    print(f"# run {kind}:", " ".join(command))
    completed = subprocess.run(
        command, check=False, text=True,
        stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
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
        print(output, end="" if output.endswith("\n") else "\n",
              file=sys.stderr)
    return True


def module_script(name, *arguments):
    command = ["sudo", str(PMU_MODULE_DIR / name),
               *(str(value) for value in arguments)]
    print("# module:", " ".join(command))
    completed = subprocess.run(command, check=False)
    if completed.returncode != 0:
        raise RuntimeError(
            f"{' '.join(command)} exited with {completed.returncode}"
        )


def load_pmu_module(args):
    module_dir = Path("/sys/module") / PMU_MODULE_NAME
    if module_dir.is_dir():
        raise RuntimeError(
            f"{PMU_MODULE_NAME} is already loaded; refusing to replace it"
        )
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
            cases.setdefault(section, {})
            continue
        if section is None:
            continue
        match = META_RE.match(line)
        if match:
            cases[section].update({
                "train_mode": int(match.group(1)),
                "probe_delta_lines": int(match.group(2)),
                "rounds": int(match.group(3)),
            })
            continue
        match = PMU_RE.match(line)
        if match:
            cases[section][match.group(1)] = {
                "total": int(match.group(2)),
                "per_trigger": float(match.group(3)),
            }
            continue
        match = CACHE_RE.match(line)
        if match:
            cases[section].update({
                "probes": int(match.group(1)),
                "hits": int(match.group(2)),
                "hits_per_1000": int(match.group(3)),
                "avg_latency_ns": float(match.group(4)),
                "threshold_ns": int(match.group(5)),
            })
    return cases


def merged_table(pmu_text, cache_text):
    pmu_cases = parse_output(pmu_text)
    cache_cases = parse_output(cache_text)
    remaining = (set(pmu_cases) | set(cache_cases)) - set(CASE_ORDER)
    order = [*CASE_ORDER, *sorted(remaining)]
    header = (
        "case\ttrain_mode\tprobe_delta_lines\trounds\tprobes\thits\t"
        "hits_per_1000\tavg_latency_ns\tl1_total\tl1_per_trigger\t"
        "l2_total\tl2_per_trigger"
    )
    lines = ["# BOP cache-probe and PMU results from separate runs", header]
    for case in order:
        cache = cache_cases.get(case, {})
        pmu = pmu_cases.get(case, {})
        if not cache and not pmu:
            continue
        l1 = pmu.get("l1d_cache_hwprf", {})
        l2 = pmu.get("l2d_cache_hwprf", {})
        values = [
            case,
            cache.get("train_mode", pmu.get("train_mode", "-")),
            cache.get("probe_delta_lines", pmu.get("probe_delta_lines", "-")),
            cache.get("rounds", pmu.get("rounds", "-")),
            cache.get("probes", "-"),
            cache.get("hits", "-"),
            cache.get("hits_per_1000", "-"),
            (f'{cache["avg_latency_ns"]:.2f}'
             if "avg_latency_ns" in cache else "-"),
            l1.get("total", "-"),
            (f'{l1["per_trigger"]:.6f}'
             if "per_trigger" in l1 else "-"),
            l2.get("total", "-"),
            (f'{l2["per_trigger"]:.6f}'
             if "per_trigger" in l2 else "-"),
        ]
        lines.append("\t".join(str(value) for value in values))
    return "\n".join(lines) + "\n"


def print_green(message):
    if sys.stdout.isatty():
        print(f"\033[32m{message}\033[0m")
    else:
        print(message)


def print_summary(pmu_text, cache_text):
    pmu = parse_output(pmu_text)
    cache = parse_output(cache_text)
    expected = "fixed_offset_expected"
    untrained = "untrained"

    if expected in cache and untrained in cache:
        hit_delta = (
            cache[expected].get("hits_per_1000", 0)
            - cache[untrained].get("hits_per_1000", 0)
        )
        latency_delta = (
            cache[expected].get("avg_latency_ns", 0.0)
            - cache[untrained].get("avg_latency_ns", 0.0)
        )
        print_green(
            "# delta fixed_expected-vs-untrained: "
            f"hits_per_1000={hit_delta:+d} "
            f"avg_latency_ns={latency_delta:+.2f}"
        )

    for event in ("l1d_cache_hwprf", "l2d_cache_hwprf"):
        if expected not in pmu or untrained not in pmu:
            continue
        trained = pmu[expected].get(event, {}).get("per_trigger", 0.0)
        control = pmu[untrained].get(event, {}).get("per_trigger", 0.0)
        print_green(
            f"# delta {event} fixed_expected-untrained="
            f"{trained - control:+.6f}/trigger "
            f"(fixed={trained:.6f}, untrained={control:.6f})"
        )


def output_paths(args):
    if args.output:
        combined = Path(args.output)
    else:
        RESULT_DIR.mkdir(parents=True, exist_ok=True)
        combined = RESULT_DIR / (
            f"bop-{args.arch}-cpu{args.core}-thr{args.threshold_ns}-"
            f"pairs{args.train_pairs}.txt"
        )
    combined.parent.mkdir(parents=True, exist_ok=True)
    return (
        combined,
        combined.with_name(combined.stem + "-pmu" + combined.suffix),
        combined.with_name(combined.stem + "-cache" + combined.suffix),
    )


def save_outputs(args, outputs):
    combined, pmu_path, cache_path = output_paths(args)
    if "pmu" in outputs:
        pmu_path.write_text(outputs["pmu"])
        print(f"# output PMU: {pmu_path}")
    if "cache" in outputs:
        cache_path.write_text(outputs["cache"])
        print(f"# output cache: {cache_path}")
    combined.write_text(merged_table(
        outputs.get("pmu", ""), outputs.get("cache", "")
    ))
    print(f"# output merged table: {combined}")


def plot_results(args, pmu_text, cache_text):
    if args.no_plot:
        return
    pmu = parse_output(pmu_text)
    cache = parse_output(cache_text)
    if not pmu and not cache:
        return
    try:
        os.environ.setdefault("MPLCONFIGDIR", "/tmp/matplotlib-cache")
        import matplotlib
        matplotlib.use("Agg")
        import matplotlib.pyplot as plt
    except ImportError:
        print("# matplotlib unavailable; skip plot", file=sys.stderr)
        return

    cases = [
        case for case in ("fixed_offset_expected",)
        if case in pmu or case in cache
    ]
    labels = [case.replace("_", "\n") for case in cases]
    positions = list(range(len(cases)))
    figure, axes = plt.subplots(2, 1, figsize=(10, 6),
                                constrained_layout=True)
    axes[0].bar(positions, [
        cache.get(case, {}).get("hits_per_1000", 0) for case in cases
    ])
    axes[0].set_xticks(positions, labels)
    axes[0].set_ylabel("Cache hits / 1000")
    axes[0].set_ylim(0, max(1000, axes[0].get_ylim()[1]))
    width = 0.35
    axes[1].bar(
        [position - width / 2 for position in positions],
        [pmu.get(case, {}).get("l1d_cache_hwprf", {})
         .get("per_trigger", 0.0) for case in cases],
        width=width, label="L1 HW prefetch",
    )
    axes[1].bar(
        [position + width / 2 for position in positions],
        [pmu.get(case, {}).get("l2d_cache_hwprf", {})
         .get("per_trigger", 0.0) for case in cases],
        width=width, label="L2 HW prefetch",
    )
    axes[1].set_xticks(positions, labels)
    axes[1].set_ylabel("PMU events / trigger")
    axes[1].set_ylim(0, max(1, axes[1].get_ylim()[1]))
    axes[1].legend(fontsize=8)
    axes[0].set_title(f"BOP test: {args.arch} CPU{args.core}")
    for axis in axes:
        axis.grid(axis="y", alpha=0.25)
        axis.tick_params(axis="x", labelsize=7)
    path = (Path(args.plot) if args.plot
            else output_paths(args)[0].with_suffix(".png"))
    path.parent.mkdir(parents=True, exist_ok=True)
    figure.savefig(path, dpi=300)
    plt.close(figure)
    print(f"# plot: {path}")


def parsed_sweep(samples):
    return [
        (
            train_pairs,
            parse_output(outputs.get("pmu", "")),
            parse_output(outputs.get("cache", "")),
        )
        for train_pairs, outputs in samples
    ]


def sweep_output_paths(args):
    if args.output:
        table_path = Path(args.output)
    else:
        RESULT_DIR.mkdir(parents=True, exist_ok=True)
        table_path = RESULT_DIR / (
            f"bop-{args.arch}-cpu{args.core}-thr{args.threshold_ns}-"
            "train-pairs-sweep.tsv"
        )
    plot_path = (Path(args.plot) if args.plot
                 else table_path.with_suffix(".png"))
    table_path.parent.mkdir(parents=True, exist_ok=True)
    return table_path, plot_path


def save_sweep_results(args, samples):
    header = (
        "train_pairs\tcase\thits_per_1000\tavg_latency_ns\t"
        "l1_per_trigger\tl2_per_trigger"
    )
    lines = ["# BOP train-pairs sweep", header]
    for train_pairs, pmu, cache in parsed_sweep(samples):
        for case in CASE_ORDER:
            cache_case = cache.get(case, {})
            pmu_case = pmu.get(case, {})
            if not cache_case and not pmu_case:
                continue
            l1 = pmu_case.get("l1d_cache_hwprf", {})
            l2 = pmu_case.get("l2d_cache_hwprf", {})
            values = [
                train_pairs,
                case,
                cache_case.get("hits_per_1000", "-"),
                (f'{cache_case["avg_latency_ns"]:.2f}'
                 if "avg_latency_ns" in cache_case else "-"),
                (f'{l1["per_trigger"]:.6f}'
                 if "per_trigger" in l1 else "-"),
                (f'{l2["per_trigger"]:.6f}'
                 if "per_trigger" in l2 else "-"),
            ]
            lines.append("\t".join(str(value) for value in values))
    table_path, _ = sweep_output_paths(args)
    table_path.write_text("\n".join(lines) + "\n")
    print(f"# output train-pairs sweep: {table_path}")


def plot_sweep_results(args, samples):
    if args.no_plot:
        return
    parsed = parsed_sweep(samples)
    if not parsed:
        return
    try:
        os.environ.setdefault("MPLCONFIGDIR", "/tmp/matplotlib-cache")
        import matplotlib
        matplotlib.use("Agg")
        import matplotlib.pyplot as plt
    except ImportError:
        print("# matplotlib unavailable; skip sweep plot", file=sys.stderr)
        return

    train_pairs_values = [item[0] for item in parsed]
    figure, axes = plt.subplots(2, 1, figsize=(10, 7), sharex=True,
                                constrained_layout=True)
    case = "fixed_offset_expected"
    cache_values = []
    cache_available = False
    for _, _, cache in parsed:
        case_data = cache.get(case, {})
        cache_values.append(case_data.get("hits_per_1000", 0))
        cache_available |= "hits_per_1000" in case_data
    if cache_available:
        axes[0].plot(train_pairs_values, cache_values, marker="o",
                     markersize=3, linewidth=1.5,
                     label="fixed offset expected")

    for event, label in (
        ("l1d_cache_hwprf", "L1 HW prefetch"),
        ("l2d_cache_hwprf", "L2 HW prefetch"),
    ):
        values = []
        available = False
        for _, pmu, _ in parsed:
            event_data = pmu.get(case, {}).get(event, {})
            values.append(event_data.get("per_trigger", 0.0))
            available |= "per_trigger" in event_data
        if available:
            axes[1].plot(train_pairs_values, values, marker="o",
                         markersize=3, linewidth=1.5, label=label)

    axes[0].set_ylabel("Cache hits / 1000")
    axes[1].set_ylabel("PMU events / trigger")
    for axis in axes:
        axis.grid(axis="both", alpha=0.25)
        if axis.lines:
            axis.legend(fontsize=8)

    axes[0].set_ylim(0, max(1000, axes[0].get_ylim()[1]))
    axes[1].set_ylim(0, max(1, axes[1].get_ylim()[1]))
    axes[1].set_xlabel("Training pairs per round")
    axes[1].set_xticks(TRAIN_PAIR_SWEEP[::2] + (TRAIN_PAIR_SWEEP[-1],))
    axes[0].set_title(
        f"BOP train-pairs sweep: {args.arch} CPU{args.core}"
    )
    _, plot_path = sweep_output_paths(args)
    plot_path.parent.mkdir(parents=True, exist_ok=True)
    figure.savefig(plot_path, dpi=300)
    plt.close(figure)
    print(f"# sweep plot: {plot_path}")


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

    samples = []
    pmu_loaded = False
    try:
        if args.pmu:
            load_pmu_module(args)
            pmu_loaded = True

        train_pairs_values = (TRAIN_PAIR_SWEEP if args.sweep_train_pairs
                              else (args.train_pairs,))
        for train_pairs in train_pairs_values:
            args.train_pairs = train_pairs
            outputs = {}
            if args.sweep_train_pairs:
                print(f"\n# train-pairs sweep: {train_pairs}")

            if args.pmu:
                code, text = run_binary(args, binaries["pmu"], "PMU-only")
                if report_failure("PMU-only", code, text):
                    return 128 - code if code < 0 else code
                outputs["pmu"] = text
                print(text, end="" if text.endswith("\n") else "\n")

            if args.cache_probe:
                code, text = run_binary(
                    args, binaries["cache"], "cache-only"
                )
                if report_failure("cache-only", code, text):
                    return 128 - code if code < 0 else code
                outputs["cache"] = text
                print(text, end="" if text.endswith("\n") else "\n")

            samples.append((train_pairs, outputs))
            print_summary(outputs.get("pmu", ""),
                          outputs.get("cache", ""))
            if not args.sweep_train_pairs:
                save_outputs(args, outputs)
                plot_results(args, outputs.get("pmu", ""),
                             outputs.get("cache", ""))

        if args.sweep_train_pairs:
            save_sweep_results(args, samples)
            plot_sweep_results(args, samples)
        return 0
    finally:
        if pmu_loaded:
            try:
                unload_pmu_module()
            except RuntimeError as exc:
                print(f"error: PMU module unload failed: {exc}",
                      file=sys.stderr)


if __name__ == "__main__":
    raise SystemExit(main())
