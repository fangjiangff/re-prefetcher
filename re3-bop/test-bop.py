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
BINARY_STEM = "test-bop"
RESULT_STEM = "bop"
DISPLAY_NAME = "BOP"
TRAINING_ACCESS_TYPES = ("load", "store")
RUNNER_DESCRIPTION = (
    "Build and run the strict BOP existence test with untrained, "
    "matched random-delta, fixed-offset, and wrong-target cases."
)

ARCH_CONFIG = {
    "X925": {"core": 6, "threshold_ns": 80},
    "A725": {"core": 4, "threshold_ns": 100},
}

CASE_ORDER = [
    "untrained",
    "random_delta_control",
    "fixed_offset_expected",
    # "trained_wrong_target",
]
TRAIN_PAIR_SWEEP = (0, *range(0, 2000, 50))
OFFSET_SWEEP = tuple(range(0, 129))
DEFAULT_OFFSET_LINES = 16
MAX_OFFSET_LINES = OFFSET_SWEEP[-1]
LINES_PER_PAGE = 64
POOL_PAGES = 8192
PRE_TRIGGER_ACTIONS = {
    "none": 0,
    "syscall": 1,
    "yield": 2,
    "nanosleep": 3,
    "cpp_rctx": 4,
    "process": 5,
}

PMU_RE = re.compile(
    r"^# PMU\s+(\S+)\s+total=(\d+)\s+per_trigger=([0-9.]+)$"
)
CACHE_RE = re.compile(
    r"^# CACHE\s+probes=(\d+)\s+hits=(\d+)\s+"
    r"hits_per_1000=(\d+)\s+avg_latency_ns=([0-9.]+)\s+"
    r"threshold_ns=(\d+)$"
)
META_RE = re.compile(
    r"^# train_mode=(\d+)\s+"
    r"(?:train_offset_lines=(\d+|-)\s+)?"
    r"probe_delta_lines=(\d+)\s+rounds=(\d+)$"
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
        description=RUNNER_DESCRIPTION
    )
    parser.add_argument("--arch", choices=ARCH_CONFIG, default="X925")
    parser.add_argument("--core", "--cpu", dest="core", type=int)
    parser.add_argument("--rounds", type=int, default=100)
    parser.add_argument("--threshold-ns", type=int)
    parser.add_argument("--train-pairs", type=int, default=1024)
    parser.add_argument("--type", choices=TRAINING_ACCESS_TYPES, default="load",
                        help="Training and trigger access type (probes remain loads).")
    parser.add_argument(
        "--offset", "--offset-lines", dest="offset_lines", type=int,
        default=DEFAULT_OFFSET_LINES,
        help="BOP train/probe offset in cache lines.",
    )
    parser.add_argument(
        "--sweep-train-pairs", action="store_true",
        help="Run train-pairs=0,100,200,...,3000 and generate a line plot.",
    )
    parser.add_argument(
        "--sweep-offsets", action="store_true",
        help="Run offset=0,1,...,128 and generate a summary table.",
    )
    parser.add_argument("--trigger-batch", type=int, default=1)
    parser.add_argument(
        "--pre-trigger-action", choices=PRE_TRIGGER_ACTIONS, default="none",
        help=(
            "Action to run after training/setup and before trigger accesses: "
            "none, syscall, yield, nanosleep, cpp_rctx, or process."
        ),
    )
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
    if args.sweep_train_pairs and args.sweep_offsets:
        parser.error("choose at most one of --sweep-train-pairs and --sweep-offsets")
    positive = ("rounds", "threshold_ns", "trigger_batch")
    if any(getattr(args, name) <= 0 for name in positive):
        parser.error(
            "rounds, threshold-ns, and trigger-batch must be positive"
        )
    if args.train_pairs < 0:
        parser.error("train-pairs must be non-negative")
    if args.core < 0 or args.wait_nops < 0:
        parser.error("core and wait-nops must be non-negative")
    if args.offset_lines < 0:
        parser.error("offset must be non-negative")
    if args.offset_lines > MAX_OFFSET_LINES:
        parser.error(f"offset must be at most {MAX_OFFSET_LINES} cache lines")
    max_train_pairs = (TRAIN_PAIR_SWEEP[-1] if args.sweep_train_pairs
                       else args.train_pairs)
    max_offset_lines = (OFFSET_SWEEP[-1] if args.sweep_offsets
                        else args.offset_lines)
    span_pages = max_offset_lines // LINES_PER_PAGE + 1
    slots_needed = max_train_pairs + args.trigger_batch
    if slots_needed * span_pages > POOL_PAGES:
        parser.error(
            "train-pairs plus trigger pages exceeds mapped pool "
            f"({slots_needed * span_pages} > {POOL_PAGES})"
        )
    if not args.pmu and not args.cache_probe:
        parser.error("enable at least one of --pmu and --cache-probe")
    return args


def binary_path(kind, access_type="load"):
    suffix = "-store" if access_type == "store" else ""
    return BIN_DIR / f"{BINARY_STEM}-{kind}{suffix}"


def build(args, kind, enable_pmu, enable_cache_probe):
    output = binary_path(kind, args.type)
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
        f"-DTRAIN_STORE={int(args.type == 'store')}",
        f"-DPMU_CORE_X925={int(args.arch == 'X925')}",
        str(SRC),
        str(UTIL_SRC),
        "-o", str(output),
    ]
    print(f"# build {kind}:", " ".join(str(item) for item in command))
    subprocess.run(command, check=True)
    return output


def run_arguments(args):
    arguments = [
        str(args.rounds),
        str(args.threshold_ns),
        str(args.train_pairs),
        str(args.trigger_batch),
        str(args.offset_lines),
    ]
    if args.pre_trigger_action != "none":
        arguments.append(str(PRE_TRIGGER_ACTIONS[args.pre_trigger_action]))
    return arguments


def run_binary(args, binary, kind):
    if platform.machine().lower() not in ("aarch64", "arm64"):
        raise RuntimeError("execution requires an AArch64 host; use --no-run")
    command = [
        "taskset", "-c", str(args.core), str(binary), *run_arguments(args)
    ]
    print(f"# run {kind} offset_lines={args.offset_lines}:", " ".join(command))
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
        target_path = module_dir / "parameters" / "target_cpu"
        target_cpu = int(target_path.read_text().strip(), 0)
        if target_cpu == args.core:
            validate_pmu_module(args)
            print(f"# direct PMU module already targets CPU{args.core}; reuse it")
            return False
        print(
            f"# direct PMU module targets CPU{target_cpu}; "
            f"unload it before binding CPU{args.core}"
        )
        module_script("module_unload.sh")
    module_script("module_load.sh", "--cpu", args.core)
    try:
        validate_pmu_module(args)
    except Exception:
        module_script("module_unload.sh")
        raise
    print(f"# direct PMU module targets CPU{args.core}")
    return True


def validate_pmu_module(args):
    module_dir = Path("/sys/module") / PMU_MODULE_NAME
    target_path = module_dir / "parameters" / "target_cpu"
    target_cpu = int(target_path.read_text().strip(), 0)
    if target_cpu != args.core:
        raise RuntimeError(
            f"PMU module targets CPU{target_cpu}, expected CPU{args.core}"
        )
    perf_access = Path("/proc/sys/kernel/perf_user_access")
    if perf_access.is_file() and perf_access.read_text().strip() != "1":
        raise RuntimeError("kernel.perf_user_access must be 1")


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
            train_offset = match.group(2)
            cases[section].update({
                "train_mode": int(match.group(1)),
                "train_offset_lines": (
                    None if train_offset == "-" else
                    int(train_offset) if train_offset is not None else
                    int(match.group(3))
                ),
                "probe_delta_lines": int(match.group(3)),
                "rounds": int(match.group(4)),
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
        "case\ttrain_mode\ttrain_offset_lines\tprobe_delta_lines\t"
        "rounds\tprobes\thits\t"
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
            format_optional(cache.get(
                "train_offset_lines", pmu.get("train_offset_lines")
            )),
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
    type_part = "-store" if args.type == "store" else ""
    offset_part = f"-off{args.offset_lines}"
    action_part = (
        "" if args.pre_trigger_action == "none"
        else f"-pre{args.pre_trigger_action}"
    )
    if args.output:
        combined = Path(args.output)
    else:
        RESULT_DIR.mkdir(parents=True, exist_ok=True)
        combined = RESULT_DIR / (
            f"{RESULT_STEM}-{args.arch}-cpu{args.core}-thr{args.threshold_ns}-"
            f"pairs{args.train_pairs}{offset_part}{action_part}{type_part}.txt"
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
    axes[0].set_title(f"{DISPLAY_NAME}: {args.arch} CPU{args.core}")
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
    type_part = "-store" if args.type == "store" else ""
    offset_part = f"-off{args.offset_lines}"
    action_part = (
        "" if args.pre_trigger_action == "none"
        else f"-pre{args.pre_trigger_action}"
    )
    if args.output:
        table_path = Path(args.output)
    else:
        RESULT_DIR.mkdir(parents=True, exist_ok=True)
        table_path = RESULT_DIR / (
            f"{RESULT_STEM}-{args.arch}-cpu{args.core}-thr{args.threshold_ns}-"
            f"train-pairs-sweep{offset_part}{action_part}{type_part}.tsv"
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
    axes[1].set_xticks(TRAIN_PAIR_SWEEP[::2])
    axes[0].set_title(
        f"{DISPLAY_NAME} train-pairs sweep: {args.arch} CPU{args.core}"
    )
    _, plot_path = sweep_output_paths(args)
    plot_path.parent.mkdir(parents=True, exist_ok=True)
    figure.savefig(plot_path, dpi=300)
    plt.close(figure)
    print(f"# sweep plot: {plot_path}")


def offset_sweep_output_path(args):
    type_part = "-store" if args.type == "store" else ""
    if args.output:
        path = Path(args.output)
    else:
        RESULT_DIR.mkdir(parents=True, exist_ok=True)
        suffix = "pmu-offset-sweep" if args.pmu and not args.cache_probe else "offset-sweep"
        action_part = (
            "" if args.pre_trigger_action == "none"
            else f"-pre{args.pre_trigger_action}"
        )
        path = RESULT_DIR / (
            f"{RESULT_STEM}-{args.arch}-cpu{args.core}-thr{args.threshold_ns}-"
            f"pairs{args.train_pairs}-{suffix}-off{OFFSET_SWEEP[0]}-{OFFSET_SWEEP[-1]}"
            f"{action_part}{type_part}.tsv"
        )
    path.parent.mkdir(parents=True, exist_ok=True)
    return path


def offset_sweep_plot_path(args):
    if args.plot:
        path = Path(args.plot)
    else:
        path = offset_sweep_output_path(args).with_suffix(".png")
    path.parent.mkdir(parents=True, exist_ok=True)
    return path


def offset_sweep_delta_plot_path(args):
    path = offset_sweep_plot_path(args)
    return path.with_name(path.stem + "-delta" + path.suffix)


def format_optional(value, places=None):
    if value is None:
        return "-"
    if places is None:
        return str(value)
    return f"{value:.{places}f}"


def offset_sweep_row(offset, pmu, cache):
    untrained = cache.get("untrained", {})
    random_control = cache.get("random_delta_control", {})
    fixed = cache.get("fixed_offset_expected", {})
    wrong_target = cache.get("trained_wrong_target", {})
    untrained_hits = untrained.get("hits_per_1000")
    fixed_hits = fixed.get("hits_per_1000")
    untrained_latency = untrained.get("avg_latency_ns")
    fixed_latency = fixed.get("avg_latency_ns")
    hit_delta = (
        fixed_hits - untrained_hits
        if fixed_hits is not None and untrained_hits is not None else None
    )
    latency_delta = (
        fixed_latency - untrained_latency
        if fixed_latency is not None and untrained_latency is not None
        else None
    )
    observed = (
        "-" if offset == 0 else
        int(hit_delta > 0) if hit_delta is not None else "-"
    )

    pmu_untrained = pmu.get("untrained", {})
    pmu_random = pmu.get("random_delta_control", {})
    pmu_fixed = pmu.get("fixed_offset_expected", {})
    pmu_wrong = pmu.get("trained_wrong_target", {})
    l1_untrained = pmu_untrained.get("l1d_cache_hwprf", {}).get("per_trigger")
    l1_fixed = pmu_fixed.get("l1d_cache_hwprf", {}).get("per_trigger")
    l2_untrained = pmu_untrained.get("l2d_cache_hwprf", {}).get("per_trigger")
    l2_fixed = pmu_fixed.get("l2d_cache_hwprf", {}).get("per_trigger")
    l1_delta = (
        l1_fixed - l1_untrained
        if l1_fixed is not None and l1_untrained is not None else None
    )
    l2_delta = (
        l2_fixed - l2_untrained
        if l2_fixed is not None and l2_untrained is not None else None
    )
    random_l1 = pmu_random.get("l1d_cache_hwprf", {}).get("per_trigger")
    wrong_l1 = pmu_wrong.get("l1d_cache_hwprf", {}).get("per_trigger")
    random_l2 = pmu_random.get("l2d_cache_hwprf", {}).get("per_trigger")
    wrong_l2 = pmu_wrong.get("l2d_cache_hwprf", {}).get("per_trigger")

    return [
        offset,
        observed,
        format_optional(untrained_hits),
        format_optional(fixed_hits),
        format_optional(hit_delta),
        format_optional(untrained_latency, 2),
        format_optional(fixed_latency, 2),
        format_optional(latency_delta, 2),
        format_optional(l1_delta, 6),
        format_optional(l2_delta, 6),
        format_optional(random_control.get("hits_per_1000")),
        format_optional(wrong_target.get("hits_per_1000")),
        format_optional(random_control.get("avg_latency_ns"), 2),
        format_optional(wrong_target.get("avg_latency_ns"), 2),
        format_optional(
            fixed_hits - random_control.get("hits_per_1000")
            if fixed_hits is not None and
            random_control.get("hits_per_1000") is not None else None
        ),
        format_optional(
            wrong_target.get("hits_per_1000") - untrained_hits
            if wrong_target.get("hits_per_1000") is not None and
            untrained_hits is not None else None
        ),
        format_optional(random_l1, 6),
        format_optional(wrong_l1, 6),
        format_optional(random_l2, 6),
        format_optional(wrong_l2, 6),
        "degenerate_same_address" if offset == 0 else "measurable",
    ]


def parsed_offset_sweep(samples):
    return [
        (
            offset,
            parse_output(outputs.get("pmu", "")),
            parse_output(outputs.get("cache", "")),
        )
        for offset, outputs in samples
    ]


def save_offset_sweep_results(args, samples):
    header = (
        "offset_lines\tobserved\tuntrained_hits_per_1000\t"
        "fixed_hits_per_1000\tdelta_hits_per_1000\t"
        "untrained_avg_latency_ns\tfixed_avg_latency_ns\t"
        "delta_avg_latency_ns\tl1_delta_per_trigger\tl2_delta_per_trigger\t"
        "random_hits_per_1000\twrong_target_hits_per_1000\t"
        "random_avg_latency_ns\twrong_target_avg_latency_ns\t"
        "fixed_minus_random_hits_per_1000\t"
        "wrong_minus_untrained_hits_per_1000\trandom_l1_per_trigger\t"
        "wrong_target_l1_per_trigger\trandom_l2_per_trigger\t"
        "wrong_target_l2_per_trigger\tinterpretation"
    )
    lines = ["# BOP offset sweep", header]
    observed_offsets = []
    missing_offsets = []
    for offset, pmu, cache in parsed_offset_sweep(samples):
        row = offset_sweep_row(offset, pmu, cache)
        if row[1] == 1:
            observed_offsets.append(offset)
        elif row[1] == 0:
            missing_offsets.append(offset)
        lines.append("\t".join(str(value) for value in row))

    path = offset_sweep_output_path(args)
    path.write_text("\n".join(lines) + "\n")
    print(f"# output offset sweep: {path}")
    if observed_offsets or missing_offsets:
        print_green(
            "# offset sweep observed: "
            f"{len(observed_offsets)}/{len(observed_offsets) + len(missing_offsets)} "
            "offsets have fixed_hits_per_1000 > untrained_hits_per_1000"
        )
    if missing_offsets:
        joined = ",".join(str(offset) for offset in missing_offsets)
        print(f"# offsets without positive cache delta: {joined}")


def pmu_average_row(offset, pmu):
    pmu_untrained = pmu.get("untrained", {})
    pmu_random = pmu.get("random_delta_control", {})
    pmu_fixed = pmu.get("fixed_offset_expected", {})
    pmu_wrong = pmu.get("trained_wrong_target", {})
    l1_untrained = pmu_untrained.get("l1d_cache_hwprf", {}).get("per_trigger")
    l1_fixed = pmu_fixed.get("l1d_cache_hwprf", {}).get("per_trigger")
    l2_untrained = pmu_untrained.get("l2d_cache_hwprf", {}).get("per_trigger")
    l2_fixed = pmu_fixed.get("l2d_cache_hwprf", {}).get("per_trigger")
    l1_delta = (
        l1_fixed - l1_untrained
        if l1_fixed is not None and l1_untrained is not None else None
    )
    l2_delta = (
        l2_fixed - l2_untrained
        if l2_fixed is not None and l2_untrained is not None else None
    )
    return [
        offset,
        format_optional(l1_untrained, 6),
        format_optional(l1_fixed, 6),
        format_optional(l1_delta, 6),
        format_optional(l2_untrained, 6),
        format_optional(l2_fixed, 6),
        format_optional(l2_delta, 6),
        format_optional(
            pmu_random.get("l1d_cache_hwprf", {}).get("per_trigger"), 6
        ),
        format_optional(
            pmu_wrong.get("l1d_cache_hwprf", {}).get("per_trigger"), 6
        ),
        format_optional(
            pmu_random.get("l2d_cache_hwprf", {}).get("per_trigger"), 6
        ),
        format_optional(
            pmu_wrong.get("l2d_cache_hwprf", {}).get("per_trigger"), 6
        ),
        "degenerate_same_address" if offset == 0 else "measurable",
    ]


def save_offset_pmu_results(args, samples):
    header = (
        "offset_lines\tuntrained_l1_per_trigger\tfixed_l1_per_trigger\t"
        "delta_l1_per_trigger\tuntrained_l2_per_trigger\t"
        "fixed_l2_per_trigger\tdelta_l2_per_trigger\t"
        "random_l1_per_trigger\twrong_target_l1_per_trigger\t"
        "random_l2_per_trigger\twrong_target_l2_per_trigger\tinterpretation"
    )
    lines = ["# BOP offset sweep average PMU events", header]
    for offset, pmu, _ in parsed_offset_sweep(samples):
        lines.append("\t".join(str(value) for value in pmu_average_row(offset, pmu)))

    path = offset_sweep_output_path(args)
    path.write_text("\n".join(lines) + "\n")
    print(f"# output offset PMU averages: {path}")


def plot_offset_pmu_results(args, samples):
    if args.no_plot:
        return
    parsed = parsed_offset_sweep(samples)
    if not parsed:
        return
    rows = [
        pmu_average_row(offset, pmu)
        for offset, pmu, _ in parsed
    ]
    if not any(row[1] != "-" or row[4] != "-" for row in rows):
        return
    try:
        os.environ.setdefault("MPLCONFIGDIR", "/tmp/matplotlib-cache")
        import matplotlib
        matplotlib.use("Agg")
        import matplotlib.pyplot as plt
    except ImportError:
        print("# matplotlib unavailable; skip offset PMU plot", file=sys.stderr)
        return

    def series(index):
        return [float(row[index]) if row[index] != "-" else 0.0 for row in rows]

    offsets = [row[0] for row in rows]
    figure, axis = plt.subplots(figsize=(12, 5.5),
                                constrained_layout=True)
    axis.axvspan(0, 32, color="#b7e4c7", alpha=0.32,
                 label="offset 0..32")
    axis.axhline(0, color="0.25", linewidth=0.8)
    axis.axvline(32, color="#2d6a4f", linestyle="--", linewidth=1)
    axis.axvline(33, color="#7f1d1d", linestyle=":", linewidth=1)
    axis.plot(offsets, series(2), color="#d62728", linewidth=1.4,
              label="fixed-offset L1")
    axis.plot(offsets, series(5), color="#1f77b4", linewidth=1.6,
              label="fixed-offset L2")
    axis.plot(offsets, series(1), color="#d62728", linewidth=1.0,
              linestyle="--", alpha=0.55, label="untrained L1")
    axis.plot(offsets, series(4), color="#1f77b4", linewidth=1.0,
              linestyle="--", alpha=0.55, label="untrained L2")
    axis.grid(axis="both", alpha=0.25)
    axis.legend(fontsize=8, frameon=False, loc="upper right")
    axis.set_xlabel("Offset (cache lines)")
    axis.set_ylabel("PMU events / trigger")
    axis.set_xlim(min(offsets), max(offsets))
    axis.set_title(
        f"{DISPLAY_NAME} average PMU events by offset: "
        f"{args.arch} CPU{args.core}"
    )
    path = offset_sweep_plot_path(args)
    figure.savefig(path, dpi=300)
    plt.close(figure)
    print(f"# offset PMU plot: {path}")

    figure, axis = plt.subplots(figsize=(12, 5.5),
                                constrained_layout=True)
    axis.axvspan(0, 32, color="#b7e4c7", alpha=0.32,
                 label="offset 0..32")
    axis.axhline(0, color="0.25", linewidth=0.8)
    axis.axvline(32, color="#2d6a4f", linestyle="--", linewidth=1)
    axis.axvline(33, color="#7f1d1d", linestyle=":", linewidth=1)
    axis.plot(offsets, series(6), color="#1f77b4", linewidth=1.6,
              label="L2 delta / trigger")
    axis.plot(offsets, series(3), color="#d62728", linewidth=1.3,
              label="L1 delta / trigger")
    axis.grid(axis="both", alpha=0.25)
    axis.legend(fontsize=8, frameon=False, loc="upper right")
    axis.set_xlabel("Offset (cache lines)")
    axis.set_ylabel("Delta PMU events / trigger")
    axis.set_xlim(min(offsets), max(offsets))
    axis.set_title(
        f"BOP PMU delta by offset: {args.arch} CPU{args.core}"
    )
    path = offset_sweep_delta_plot_path(args)
    figure.savefig(path, dpi=300)
    plt.close(figure)
    print(f"# offset PMU delta plot: {path}")


def main():
    args = parse_args()
    binaries = {}
    if args.pmu:
        binaries["pmu"] = binary_path("pmu", args.type) if args.no_build else build(
            args, "pmu", enable_pmu=True, enable_cache_probe=False
        )
    if args.cache_probe:
        binaries["cache"] = binary_path("cache", args.type) if args.no_build else build(
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
            pmu_loaded = load_pmu_module(args)

        if args.sweep_offsets:
            original_offset = args.offset_lines
            for offset in OFFSET_SWEEP:
                args.offset_lines = offset
                outputs = {}
                print(f"\n# offset sweep: {offset}")

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

                samples.append((offset, outputs))
                print_summary(outputs.get("pmu", ""),
                              outputs.get("cache", ""))

            args.offset_lines = original_offset
            if args.pmu and not args.cache_probe:
                save_offset_pmu_results(args, samples)
                plot_offset_pmu_results(args, samples)
            else:
                save_offset_sweep_results(args, samples)
            return 0

        train_pairs_values = (TRAIN_PAIR_SWEEP if args.sweep_train_pairs
                              else (args.train_pairs,))
        for train_pairs in train_pairs_values:
            args.train_pairs = train_pairs
            outputs = {}
            if args.sweep_train_pairs:
                print(f"\n# train-pairs sweep: {train_pairs} offset_lines={args.offset_lines}")

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
