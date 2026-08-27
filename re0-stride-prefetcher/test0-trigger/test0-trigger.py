import argparse
import csv
import os
import subprocess
import sys

ROOT_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
if ROOT_DIR not in sys.path:
    sys.path.insert(0, ROOT_DIR)

from cross_test_config import (
    apply_single_core_defaults,
    apply_threshold_defaults,
    apply_timer_default,
    default_timer_for_arch,
    arch_choices,
    is_x86_arch,
    timer_define_for_arch,
    timer_unit_for_arch,
)

os.environ.setdefault("MPLCONFIGDIR", "/tmp/matplotlib-cache")

BASE_DIR = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.join(BASE_DIR, "test0-trigger.c")
UTIL_SRC = os.path.join(ROOT_DIR, "until.c")
OUT = os.path.join(ROOT_DIR, "bin", "test0-trigger")
RESULT_DIR = os.path.join(ROOT_DIR, "res", "store-stride")
RAW_DIR = os.path.join(RESULT_DIR, "raw")
GREEN = "\033[32m"
RESET = "\033[0m"


def parse_args():
    parser = argparse.ArgumentParser(
        description=("Sweep store-stride trigger conditions and plot either the first "
                     "candidate latency or a hardware-prefetch PMU value.")
    )
    parser.add_argument("--arch", required=True, choices=arch_choices(),
                        help="Architecture label used to select default core.")
    parser.add_argument("--core", type=int, default=None,
                        help="Override CPU core used by taskset. Default is selected from --arch.")
    parser.add_argument("--rounds", type=int, default=1000,
                        help="Rounds per stride/access-count point. Default: 100")
    parser.add_argument("--max-stride", type=int, default=32,
                        help=("Maximum stride in 64-byte cache lines. "
                              "For example, 1 means 64 bytes. Default: 32"))
    parser.add_argument("--min-step", type=int, default=1,
                        help="Minimum access count. Default: 1")
    parser.add_argument("--max-step", type=int, default=20,
                        help="Maximum access count. Default: 10")
    parser.add_argument("--access", choices=["store", "load", "prefetch"],
                        default="store",
                        help=("Access instruction used for training/trigger. "
                              "prefetch uses the architecture-specific "
                              "prefetch instruction. Default: store"))
    parser.add_argument("--hit-threshold-ns", dest="threshold_ns", type=int, default=None,
                        help="Latency <= this value is treated as prefetched. "
                             "Default is selected from --arch.")
    parser.add_argument("--timer", choices=["gettime", "rdtsc", "cntvct", "pmccntr"],
                        default=None,
                        help="Timestamp source. Default is selected from --arch.")
    parser.add_argument("--cc", default=os.environ.get("CC", "gcc"),
                        help="Compiler command. Default: $CC or gcc")
    parser.add_argument("--enable-pmu", choices=[0, 1], type=int, default=0,
                        help=("Use a hardware-prefetch PMU value instead of probing "
                              "candidate positions' latency. X925 uses "
                              "l2d_cache_hwprf. Default: 0"))
    parser.add_argument("--probe-candidates", type=int, default=10,
                        help=("Number of consecutive predicted positions sampled "
                              "independently in latency mode. Must be >= 10. "
                              "Default: 10"))
    parser.add_argument("--min-stride", type=int, default=0,
                        help=("Initial stride in 64-byte cache lines. "
                              "For example, 1 means 64 bytes. Default: 0"))
    parser.add_argument("--output", default=None,
                        help="TSV output path. Default includes arch/core/access.")
    parser.add_argument("--raw-output", default=None,
                        help="Raw program output path. Default includes arch/core/access.")
    parser.add_argument("--plot-output", default=None,
                        help="Heatmap output path. Default includes arch/core/access.")
    parser.add_argument("--plot-only", action="store_true",
                        help="Read --output and generate the heatmap without compiling/running.")
    parser.add_argument("--no-plot", action="store_true")
    parser.add_argument("--no-compile", action="store_true")
    args = parser.parse_args()

    apply_single_core_defaults(args)
    apply_threshold_defaults(args)
    apply_timer_default(args)

    if args.core < 0:
        parser.error("--core must be >= 0")
    supported_timers = {"gettime", "rdtsc"} if is_x86_arch(args.arch) else {
        "gettime", "cntvct", "pmccntr"
    }
    if args.timer not in supported_timers:
        parser.error(f"--timer {args.timer} is not supported for {args.arch}")
    if args.enable_pmu and is_x86_arch(args.arch):
        parser.error("--enable-pmu=1 requires an Arm architecture")
    if args.rounds < 1:
        parser.error("--rounds must be >= 1")
    if not args.enable_pmu and args.probe_candidates < 10:
        parser.error("--probe-candidates must be >= 10 in latency mode")
    if not args.enable_pmu and args.rounds < args.probe_candidates:
        parser.error("--rounds must be >= --probe-candidates in latency mode")
    if args.min_stride < 0:
        parser.error("--min-stride must be >= 0 (unit: 64 bytes)")
    if args.min_stride > args.max_stride:
        parser.error("--min-stride must be <= --max-stride")
    if args.max_stride < 1:
        parser.error("--max-stride must be >= 1 (unit: 64 bytes)")
    if args.min_step < 1:
        parser.error("--min-step must be >= 1")
    if args.min_step > args.max_step:
        parser.error("--min-step must be <= --max-step")
    if args.max_step < 1:
        parser.error("--max-step must be >= 1")
    if args.threshold_ns < 1:
        parser.error("--hit-threshold-ns must be >= 1")
    apply_default_paths(args)
    return args


def pmu_metric_name(arch):
    if arch == "X925":
        return "l2d_cache_hwprf"
    if arch == "A55":
        return "l2d_cache_refill_prefetch"
    return "l2d_cache_prf"


def result_name(args):
    timer_suffix = ""
    if args.timer != default_timer_for_arch(args.arch):
        timer_suffix = f"-timer{args.timer}"
    pmu_suffix = "-pmu" if args.enable_pmu else ""
    probe_suffix = "" if args.enable_pmu else f"-probes{args.probe_candidates}"
    return (
        f"{args.arch}-core{args.core}-trigger-sweep"
        f"-{args.access}-minstride{args.min_stride}lines"
        f"-maxstride{args.max_stride}lines-minstep{args.min_step}"
        f"-maxstep{args.max_step}"
        f"{timer_suffix}{pmu_suffix}{probe_suffix}"
    )


def apply_default_paths(args):
    name = result_name(args)
    if args.output is None:
        args.output = os.path.join(RESULT_DIR, f"{name}.tsv")
    if args.raw_output is None:
        args.raw_output = os.path.join(RAW_DIR, f"{name}.txt")
    if args.plot_output is None:
        args.plot_output = os.path.join(RESULT_DIR, f"{name}-heatmap.png")


def ensure_parent(path):
    parent = os.path.dirname(path)
    if parent:
        os.makedirs(parent, exist_ok=True)


def arch_cflags_for(arch):
    if is_x86_arch(arch):
        return []
    return ["-march=armv8.5-a+predres"]


def compile_test(args):
    os.makedirs(os.path.dirname(OUT), exist_ok=True)
    compile_cmd = [
        args.cc,
        "-std=gnu11",
        "-O0",
        "-static",
        f"-DROUNDS={args.rounds}",
        f"-DMAX_STRIDE={args.max_stride}",
        f"-DMAX_STEP={args.max_step}",
        f"-DMIN_STEP={args.min_step}",
        f"-DTRAIN_ACCESS_LOAD={1 if args.access == 'load' else 0}",
        f"-DTRAIN_ACCESS_PREFETCH={1 if args.access == 'prefetch' else 0}",
        f"-DENABLE_CPP_RCTX={1 if args.arch in ('X925', 'A725') else 0}",
        f"-DENABLE_PMU={args.enable_pmu}",
        f"-DPROBE_CANDIDATES={args.probe_candidates}",
        f"-DMIN_STRIDE={args.min_stride}",
        f"-DPMU_CORE_X925={1 if args.arch == 'X925' else 0}",
        f"-DPMU_CORE_A55={1 if args.arch == 'A55' else 0}",
        "-o",
        OUT,
        SRC,
        UTIL_SRC,
    ]
    timer_define = timer_define_for_arch(args.arch, args.timer)
    if timer_define is not None:
        compile_cmd.insert(-4, timer_define)
    compile_cmd[1:1] = arch_cflags_for(args.arch)
    return subprocess.run(compile_cmd)


def run_test(args):
    return subprocess.run(
        ["taskset", "-c", str(args.core), OUT],
        capture_output=True,
        text=True,
    )


def parse_rows(text, enable_pmu, probe_candidates=10):
    rows = []
    for line in text.splitlines():
        stripped = line.strip()
        if not stripped or stripped.startswith("#"):
            continue

        parts = stripped.split()
        stride_lines = int(parts[0])
        train_step = int(parts[1])
        if enable_pmu:
            if len(parts) != 3:
                raise ValueError(f"unexpected PMU output row: {line}")
            value = float(parts[2])
            best_probe_distance = None
            probe_latencies = []
        else:
            expected_fields = 4 + probe_candidates
            if len(parts) != expected_fields:
                raise ValueError(
                    f"expected {expected_fields} latency fields, got "
                    f"{len(parts)}: {line}"
                )
            best_probe_distance = int(parts[2])
            value = int(parts[3])
            probe_latencies = [int(item) for item in parts[4:]]

        rows.append({
            "stride_lines": stride_lines,
            "stride_bytes": stride_lines * 64,
            "train_step": train_step,
            "value": value,
            "best_probe_distance": best_probe_distance,
            "probe_latencies": probe_latencies,
        })

    if not rows:
        raise ValueError("no plottable result data found")
    return rows


def write_tsv(path, rows, enable_pmu, probe_candidates=10):
    ensure_parent(path)
    value_field = "pmu_per_round" if enable_pmu else "avg_ns"
    fieldnames = ["stride_lines", "stride_bytes", "train_step", value_field]
    if not enable_pmu:
        fieldnames.append("best_probe_distance")
        fieldnames.extend(
            f"probe_{distance}_avg_ns"
            for distance in range(1, probe_candidates + 1)
        )

    with open(path, "w", newline="") as f:
        writer = csv.DictWriter(f, fieldnames=fieldnames, delimiter="\t")
        writer.writeheader()
        for row in rows:
            output_row = {
                "stride_lines": row["stride_lines"],
                "stride_bytes": row["stride_bytes"],
                "train_step": row["train_step"],
                value_field: row["value"],
            }
            if not enable_pmu:
                output_row["best_probe_distance"] = row["best_probe_distance"]
                for distance, latency in enumerate(row["probe_latencies"], 1):
                    output_row[f"probe_{distance}_avg_ns"] = latency
            writer.writerow(output_row)


def read_tsv(path, enable_pmu, probe_candidates=10):
    rows = []
    with open(path, newline="") as f:
        reader = csv.DictReader(f, delimiter="\t")
        value_field = "pmu_per_round" if enable_pmu else "avg_ns"
        if value_field not in (reader.fieldnames or []):
            raise ValueError(f"TSV is missing required column: {value_field}")
        for row in reader:
            probe_fields = [
                f"probe_{distance}_avg_ns"
                for distance in range(1, probe_candidates + 1)
            ]
            has_probe_details = (
                not enable_pmu
                and all(field in (reader.fieldnames or []) for field in probe_fields)
            )
            rows.append({
                "stride_lines": int(row["stride_lines"]),
                "stride_bytes": int(row["stride_bytes"]),
                "train_step": int(row["train_step"]),
                "value": float(row[value_field]) if enable_pmu else int(row[value_field]),
                "best_probe_distance": (
                    None if enable_pmu else int(row.get("best_probe_distance") or 1)
                ),
                "probe_latencies": (
                    [int(row[field]) for field in probe_fields]
                    if has_probe_details else []
                ),
            })
    if not rows:
        raise ValueError("no plottable TSV rows found")
    return rows


def plot_heatmap(path, rows, title, unit, enable_pmu=False, pmu_metric=None):
    try:
        import matplotlib.pyplot as plt
        import numpy as np
        import seaborn as sns
        from matplotlib.colors import LinearSegmentedColormap, Normalize
    except ModuleNotFoundError as exc:
        print(f"Skipping heatmap: missing Python package '{exc.name}'.")
        print("Install plotting dependencies with:")
        print("  sudo apt install python3-matplotlib python3-seaborn")
        print("or:")
        print("  python3 -m pip install matplotlib seaborn")
        return

    ensure_parent(path)
    stride_labels = sorted({row["stride_lines"] for row in rows})
    step_labels = sorted({row["train_step"] for row in rows})
    stride_index = {value: i for i, value in enumerate(stride_labels)}
    step_index = {value: i for i, value in enumerate(step_labels)}

    matrix = np.full((len(stride_labels), len(step_labels)), np.nan)
    for row in rows:
        matrix[stride_index[row["stride_lines"]], step_index[row["train_step"]]] = row["value"]

    masked = np.ma.masked_invalid(matrix)
    if enable_pmu:
        heatmap_vmax = max(0.500001, float(np.nanmax(matrix)))
        pmu_cmap = LinearSegmentedColormap.from_list(
            "pmu_blues", ["#c6dbef", "#08306b"]
        )
        pmu_cmap.set_under("#d9d9d9")
        heatmap_options = {
            "cmap": pmu_cmap,
            "norm": Normalize(vmin=0.5, vmax=heatmap_vmax),
        }
    else:
        heatmap_options = {"cmap": "RdYlBu_r", "vmin": 0, "vmax": 200}

    width = max(8.0, 0.45 * len(step_labels) + 3.0)
    height = max(7.0, 0.18 * len(stride_labels) + 2.5)
    fig, ax = plt.subplots(figsize=(width, height))
    sns.heatmap(
        masked,
        annot=enable_pmu,
        fmt=".2f" if enable_pmu else ".0f",
        annot_kws={"fontsize": 10} if enable_pmu else None,
        ax=ax,
        cbar=True,
        cbar_kws={"pad": 0.01},
        yticklabels=False,
        **heatmap_options,
    )

    ax.set_title(title, loc="left", pad=8, fontsize=16)
    ax.set_xlabel("Access count", fontsize=14)
    ax.set_ylabel("Stride (cache lines)", fontsize=14)
    ax.set_xticks(np.arange(len(step_labels)) + 0.5)
    ax.set_xticklabels(step_labels, fontsize=12)

    y_tick_step = max(1, len(stride_labels) // 24)
    y_ticks = list(range(0, len(stride_labels), y_tick_step))
    ax.set_yticks(np.array(y_ticks) + 0.5)
    ax.set_yticklabels([stride_labels[i] for i in y_ticks], fontsize=12)

    cbar = ax.collections[0].colorbar
    if cbar is not None:
        label = (f"{pmu_metric} (events/round)" if enable_pmu
                 else f"Average reload latency ({unit})")
        cbar.set_label(label, fontsize=14)
        cbar.ax.tick_params(labelsize=12)
    fig.tight_layout()
    fig.savefig(path, dpi=300)
    plt.close(fig)


def print_summary(rows, threshold_ns, unit):
    prefetched = [row for row in rows if row["value"] <= threshold_ns]

    print(f"{GREEN}# trigger summary")
    print(f"# prefetch threshold: avg_{unit} <= {threshold_ns}")
    if not prefetched:
        print("min_access_times\tN/A")
        print("min_stride_lines\tN/A")
        print("max_stride_lines\tN/A")
        print(RESET, end="")
        return

    min_access_times = min(row["train_step"] for row in prefetched)
    min_stride_lines = min(row["stride_lines"] for row in prefetched)
    max_stride_lines = max(row["stride_lines"] for row in prefetched)

    print(f"min_access_times\t{min_access_times}")
    print(f"min_stride_lines\t{min_stride_lines}")
    print(f"max_stride_lines\t{max_stride_lines}")
    print(RESET, end="")


def main():
    args = parse_args()
    unit = timer_unit_for_arch(args.arch, args.timer)

    if args.plot_only:
        rows = read_tsv(args.output, args.enable_pmu, args.probe_candidates)
    else:
        if not args.no_compile:
            compiled = compile_test(args)
            if compiled.returncode != 0:
                print("Compile failed", file=sys.stderr)
                return compiled.returncode

        run = run_test(args)
        if run.stdout:
            print(run.stdout, end="")
        if run.stderr:
            print(run.stderr, end="", file=sys.stderr)
        if run.returncode != 0:
            print("Execution failed", file=sys.stderr)
            return run.returncode

        ensure_parent(args.raw_output)
        with open(args.raw_output, "w") as f:
            f.write(run.stdout)
        rows = parse_rows(run.stdout, args.enable_pmu, args.probe_candidates)
        write_tsv(args.output, rows, args.enable_pmu, args.probe_candidates)
        print(f"Saved TSV to {args.output}")
        print(f"Saved raw output to {args.raw_output}")

    if args.enable_pmu:
        print(f"# PMU metric: {pmu_metric_name(args.arch)} (events/round)")
    else:
        print_summary(rows, args.threshold_ns, unit)

    if not args.no_plot:
        plot_heatmap(
            args.plot_output,
            rows,
            f"{args.arch} core {args.core} {args.access} stride trigger sweep",
            unit,
            bool(args.enable_pmu),
            pmu_metric_name(args.arch),
        )
        print(f"Saved heatmap to {args.plot_output}")

    return 0


if __name__ == "__main__":
    sys.exit(main())
