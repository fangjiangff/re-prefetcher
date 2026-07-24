import argparse
import csv
import os
import subprocess
import sys

ROOT_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
if ROOT_DIR not in sys.path:
    sys.path.insert(0, ROOT_DIR)

from cross_test_config import ARCH_CONFIG, arch_choices


BASE_DIR = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.join(BASE_DIR, "test0-exist.c")
UTIL_SRC = os.path.join(ROOT_DIR, "until.c")
OUT = os.path.join(ROOT_DIR, "bin", "test0-exist")

DEFAULT_STRIDE_LINES = 5
DEFAULT_TRAIN_STEP = 3
DEFAULT_ROUNDS = 40000
DEFAULT_PROBE_POSITIONS = 100
GREEN = "\033[32m"
RED = "\033[31m"
RESET = "\033[0m"

ROLE_COLORS = {
    "accessed": "#D55E00",
    "prefetched": "#0072B2",
    "cache_miss": "#BDBDBD",
}


def parse_args():
    parser = argparse.ArgumentParser(
        description="Run test-exist and report whether the predicted line is prefetched."
    )
    parser.add_argument("--arch", choices=arch_choices(), default=None,
                        help="Architecture label. If omitted, all configured architectures are tested.")
    parser.add_argument("--core", type=int, default=None,
                        help="Override CPU core used by taskset. Default is selected from --arch.")
    parser.add_argument("--arches", default=None,
                        help="Comma-separated architecture labels.")
    parser.add_argument("--cores", default=None,
                        help="Comma-separated CPU core overrides for --arches.")
    parser.add_argument("--stride", type=int, default=DEFAULT_STRIDE_LINES,
                        help=f"Stride in cache lines. Default: {DEFAULT_STRIDE_LINES}")
    parser.add_argument("--train-step", type=int, default=DEFAULT_TRAIN_STEP,
                        help=f"Train-step for single-run mode. Default: {DEFAULT_TRAIN_STEP}")
    parser.add_argument("--rounds", type=int, default=DEFAULT_ROUNDS,
                        help=f"Rounds per test. Default: {DEFAULT_ROUNDS}")
    parser.add_argument("--probe-positions", type=int,
                        default=DEFAULT_PROBE_POSITIONS,
                        help=f"Positions to probe. Default: {DEFAULT_PROBE_POSITIONS}")
    parser.add_argument("--probe-mode", choices=["sweep", "single"],
                        default="sweep",
                        help=("Probe timing mode. sweep rotates through probe positions; "
                              "single probes only one cache line. Default: sweep"))
    parser.add_argument("--single-probe-position", type=int, default=None,
                        help=("Cache-line index for --probe-mode single. "
                              "Default: train_step * stride, the first predicted line."))
    parser.add_argument("--hit-threshold-ns", type=int,
                        default=None,
                        help=("Predicted line is treated as prefetched when "
                              "the average latency is <= this value, in the "
                              "selected timer unit. Default is selected from --arch."))
    parser.add_argument("--timer", choices=["gettime", "rdtsc", "cntvct", "pmccntr"],
                        default=None,
                        help=("Timestamp source. x86 supports gettime/rdtsc; "
                              "AArch64 supports gettime/cntvct/pmccntr. "
                              "Default is selected from --arch."))
    parser.add_argument("--access", choices=["store", "load", "prefetch"],
                        default="store",
                        help=("Stride instruction to test. "
                              "prefetch uses the architecture-specific "
                              "prefetch instruction. Default: store"))
    parser.add_argument("--dummy-access", choices=["load", "store", "prefetch"],
                        default="prefetch",
                        help="Instruction used by dummyAccesses. Default: prefetch")
    parser.add_argument("--dummy-order", choices=["sequential", "permuted"],
                        default="sequential",
                        help="Address order used by dummyAccesses. Default: sequential")
    parser.add_argument("--dummy-buffer-pages", type=int, default=64,
                        help="Number of pages in dummy_buffer. Default: 64")
    parser.add_argument("--dummy-sweep", action="store_true",
                        help="Sweep dummy access/order/page-count configurations.")
    parser.add_argument("--dummy-sweep-accesses", default="load,store,prefetch",
                        help="Comma-separated dummy accesses for --dummy-sweep.")
    parser.add_argument("--dummy-sweep-orders", default="sequential,permuted",
                        help="Comma-separated dummy orders for --dummy-sweep.")
    parser.add_argument("--dummy-sweep-pages", default="1,2,4,8,10,16,32",
                        help="Comma-separated dummy page counts for --dummy-sweep.")
    parser.add_argument("--no-trigger", action="store_true",
                        help="Skip the final same-PC trigger access after training.")
    parser.add_argument("--context-switch", action="store_true",
                        help="Call sched_yield at the post-training context-switch point.")
    parser.add_argument("--pmu-device", default=None,
                        help=("Override PMU device name passed as PMU_DEVICE. "
                              "Example: armv8_pmuv3_0"))
    parser.add_argument("--plot-only", action="store_true",
                        help="Only read existing TSV results and print summaries.")
    parser.add_argument("--no-plot", action="store_true",
                        help="Do not generate bar plots.")
    args = parser.parse_args()

    if args.core is not None and args.core < 0:
        parser.error("--core must be >= 0")
    if args.core is not None and args.arch is None:
        parser.error("--core requires --arch")
    if args.cores is not None and args.arches is None:
        parser.error("--cores requires --arches")
    if (args.arch is not None or args.core is not None) and (
        args.arches is not None or args.cores is not None
    ):
        parser.error("Use either --arch/--core or --arches/--cores, not both")
    if args.stride < 1:
        parser.error("--stride must be >= 1")
    if args.train_step < 0:
        parser.error("--train-step must be >= 0")
    if args.rounds < 1:
        parser.error("--rounds must be >= 1")
    if args.probe_positions < 1:
        parser.error("--probe-positions must be >= 1")
    if args.probe_mode == "sweep" and args.rounds < args.probe_positions:
        parser.error("--rounds must be >= --probe-positions when --probe-mode sweep")
    if args.single_probe_position is not None and args.single_probe_position < 0:
        parser.error("--single-probe-position must be >= 0")
    if args.dummy_buffer_pages < 1:
        parser.error("--dummy-buffer-pages must be >= 1")
    if args.hit_threshold_ns is not None and args.hit_threshold_ns < 1:
        parser.error("--hit-threshold-ns must be >= 1")
    requested_arches = []
    if args.arches is not None:
        requested_arches.extend(item.strip() for item in args.arches.split(",") if item.strip())
    elif args.arch is not None:
        requested_arches.append(args.arch)
    else:
        requested_arches.extend(arch_choices())
    unsupported_timer_arches = []
    if args.timer is not None:
        for arch in requested_arches:
            supported_timers = {"gettime", "rdtsc"} if arch in {"x86", "Zen4"} else {"gettime", "cntvct", "pmccntr"}
            if args.timer not in supported_timers:
                unsupported_timer_arches.append(arch)
    if unsupported_timer_arches:
        parser.error(
            f"--timer {args.timer} is not supported for: "
            + ", ".join(unsupported_timer_arches)
        )
    if args.train_step * args.stride >= args.probe_positions:
        parser.error("predicted position train_step * stride must be inside probe positions")
    if args.probe_mode == "single" and args.single_probe_position is not None:
        if args.single_probe_position >= args.probe_positions:
            parser.error("--single-probe-position must be inside probe positions")
    if args.dummy_sweep:
        valid_accesses = {"load", "store", "prefetch"}
        valid_orders = {"sequential", "permuted"}
        sweep_accesses = [item.strip() for item in args.dummy_sweep_accesses.split(",") if item.strip()]
        sweep_orders = [item.strip() for item in args.dummy_sweep_orders.split(",") if item.strip()]
        if not sweep_accesses:
            parser.error("--dummy-sweep-accesses must not be empty")
        if not sweep_orders:
            parser.error("--dummy-sweep-orders must not be empty")
        unknown_accesses = [item for item in sweep_accesses if item not in valid_accesses]
        unknown_orders = [item for item in sweep_orders if item not in valid_orders]
        if unknown_accesses:
            parser.error("unknown dummy access(es): " + ", ".join(unknown_accesses))
        if unknown_orders:
            parser.error("unknown dummy order(s): " + ", ".join(unknown_orders))
        try:
            sweep_pages = [int(item.strip()) for item in args.dummy_sweep_pages.split(",") if item.strip()]
        except ValueError:
            parser.error("--dummy-sweep-pages must contain comma-separated integers")
        if not sweep_pages:
            parser.error("--dummy-sweep-pages must not be empty")
        if any(pages < 1 for pages in sweep_pages):
            parser.error("--dummy-sweep-pages values must be >= 1")
    return args


args = parse_args()
result_dir = os.path.join(ROOT_DIR, "res", "store-stride")
plot_dir = os.path.join(ROOT_DIR, "res", "barplots")
raw_dir = os.path.join(result_dir, "raw")


def parse_csv_list(value):
    return [item.strip() for item in value.split(",") if item.strip()]


def targets():
    if args.arch is not None:
        core = args.core if args.core is not None else ARCH_CONFIG[args.arch]["core"]
        return [(args.arch, core)]

    if args.arches is not None:
        arches = parse_csv_list(args.arches)
        unknown = [arch for arch in arches if arch not in ARCH_CONFIG]
        if unknown:
            print("Error: unknown architecture(s): " + ", ".join(unknown),
                  file=sys.stderr)
            sys.exit(2)
        if args.cores is None:
            cores = [ARCH_CONFIG[arch]["core"] for arch in arches]
        else:
            try:
                cores = [int(core) for core in parse_csv_list(args.cores)]
            except ValueError:
                print("Error: --cores must contain comma-separated integers.",
                      file=sys.stderr)
                sys.exit(2)
    else:
        arches = list(arch_choices())
        cores = [ARCH_CONFIG[arch]["core"] for arch in arches]

    if len(arches) != len(cores):
        print("Error: architecture and core lists must have the same length.",
              file=sys.stderr)
        sys.exit(2)
    return list(zip(arches, cores))


def train_steps():
    return [args.train_step]


def dummy_configs():
    if not args.dummy_sweep:
        return [(args.dummy_access, args.dummy_order, args.dummy_buffer_pages)]

    accesses = parse_csv_list(args.dummy_sweep_accesses)
    orders = parse_csv_list(args.dummy_sweep_orders)
    pages = [int(item) for item in parse_csv_list(args.dummy_sweep_pages)]
    return [
        (dummy_access, dummy_order, dummy_pages)
        for dummy_access in accesses
        for dummy_order in orders
        for dummy_pages in pages
    ]



def is_x86_arch(arch):
    return arch in {"x86", "Zen4"}


def timer_for_arch(arch):
    if args.timer is not None:
        return args.timer
    configured_timer = ARCH_CONFIG[arch].get("timer")
    if configured_timer is not None:
        return configured_timer
    if is_x86_arch(arch):
        return "gettime"
    return "cntvct"


def timer_unit_for(arch):
    timer = timer_for_arch(arch)
    if timer in {"rdtsc", "pmccntr"}:
        return "cycles"
    if timer == "cntvct":
        return "ticks"
    return "ns"


def timer_define_for(arch):
    timer = timer_for_arch(arch)
    if timer == "rdtsc":
        return "-DRDTSC=1"
    if timer == "cntvct":
        return "-DCNTVCT=1"
    if timer == "pmccntr":
        return "-DPMCCNTR=1"
    return "-DGETTIME=1"


def single_probe_position(train_step):
    if args.single_probe_position is not None:
        return args.single_probe_position
    return train_step * args.stride


def probe_mode_suffix(train_step):
    if args.probe_mode != "single":
        return ""
    return f"-singleprobe{single_probe_position(train_step)}"


def arch_cflags_for(arch):
    if is_x86_arch(arch):
        return []
    return ["-march=armv8.5-a+predres"]


def pmu_defines_for_arch(arch):
    return [
        f"-DPMU_CORE_X925={1 if arch == 'X925' else 0}",
        f"-DPMU_CORE_A55={1 if arch == 'A55' else 0}",
    ]


def micro_arch_name(arch, core, train_step):
    trigger_suffix = "-no-trigger" if args.no_trigger else ""
    switch_suffix = "-ctxswitch" if args.context_switch else ""
    dummy_suffix = ""
    timer = timer_for_arch(arch)
    timer_suffix = ""
    if timer != "gettime":
        timer_suffix = f"-timer{timer}"
    if (
        args.dummy_sweep
        or args.dummy_access != "prefetch"
        or args.dummy_order != "sequential"
        or args.dummy_buffer_pages != 10
    ):
        dummy_suffix = (
            f"-dummy-{args.dummy_access}-{args.dummy_order}"
            f"-pages{args.dummy_buffer_pages}"
        )
    return (
        f"{arch}-core{core}-stride{args.stride}"
        f"-train{train_step}-{args.access}"
        f"{timer_suffix}{probe_mode_suffix(train_step)}{trigger_suffix}{switch_suffix}"
        f"{dummy_suffix}"
    )


def tsv_path_for(arch, core, train_step):
    return os.path.join(result_dir, f"{micro_arch_name(arch, core, train_step)}.tsv")


def raw_path_for(arch, core, train_step):
    return os.path.join(raw_dir, f"{micro_arch_name(arch, core, train_step)}.txt")


def plot_path_for(arch, core, train_step):
    return os.path.join(plot_dir, f"{micro_arch_name(arch, core, train_step)}.png")


def ensure_dirs():
    os.makedirs(os.path.dirname(OUT), exist_ok=True)
    os.makedirs(result_dir, exist_ok=True)
    os.makedirs(plot_dir, exist_ok=True)
    os.makedirs(raw_dir, exist_ok=True)


def accessed_offsets(train_step):
    stride_bytes = args.stride * 64
    accessed_steps = train_step - 1 if args.no_trigger else train_step
    return {step * stride_bytes for step in range(accessed_steps)}


def predicted_position(train_step):
    return train_step * args.stride


def threshold_ns_for(arch):
    if args.hit_threshold_ns is not None:
        return args.hit_threshold_ns
    return ARCH_CONFIG[arch]["threshold_ns"]


def classify_position(offset_bytes, avg_ns, probes, train_step, threshold_ns):
    if offset_bytes in accessed_offsets(train_step):
        return "accessed"
    if probes > 0 and avg_ns <= threshold_ns:
        return "prefetched"
    return "cache_miss"


def parse_output(output, train_step, threshold_ns):
    rows = []
    for line in output.splitlines():
        stripped = line.strip()
        if not stripped or stripped.startswith("#"):
            continue

        fields = stripped.split()
        if len(fields) != 4:
            print(f"Skipping unexpected output row: {line}", file=sys.stderr)
            continue

        try:
            position = int(fields[0])
            offset_bytes = int(fields[1])
            avg_ns = int(fields[2])
            probes = int(fields[3])
        except ValueError:
            print(f"Skipping non-numeric output row: {line}", file=sys.stderr)
            continue

        rows.append({
            "position": position,
            "offset_bytes": offset_bytes,
            "role": classify_position(offset_bytes, avg_ns, probes, train_step, threshold_ns),
            "avg_ns": avg_ns,
            "probes": probes,
        })
    return rows


def write_tsv(rows, path):
    with open(path, "w", newline="") as f:
        writer = csv.DictWriter(
            f,
            fieldnames=["position", "offset_bytes", "role", "avg_ns", "probes"],
            delimiter="\t",
        )
        writer.writeheader()
        writer.writerows(rows)


def read_tsv(path):
    rows = []
    with open(path, newline="") as f:
        reader = csv.DictReader(f, delimiter="\t")
        for row in reader:
            rows.append({
                "position": int(row["position"]),
                "offset_bytes": int(row["offset_bytes"]),
                "role": row["role"],
                "avg_ns": int(row["avg_ns"]),
                "probes": int(row["probes"]),
            })
    return rows


def compile_test(train_step, arch):
    compile_cmd = [
        os.environ.get("CC", "gcc"),
        "-std=gnu11",
        "-O0",
        "-static",
        # "-march=armv8.5-a+predres",
        f"-DSTRIDE_BYTES={args.stride * 64}",
        f"-DTRAIN_STEP={train_step}",
        f"-DROUNDS={args.rounds}",
        f"-DPROBE_POSITIONS={args.probe_positions}",
        f"-DSINGLE_PROBE={1 if args.probe_mode == 'single' else 0}",
        f"-DSINGLE_PROBE_POSITION={single_probe_position(train_step)}",
        f"-DCPU_ID=0",
        f"-DENABLE_CPP_RCTX={1 if arch == 'X925' else 0}",
        f"-DTRAIN_ACCESS_LOAD={1 if args.access == 'load' else 0}",
        f"-DTRAIN_ACCESS_PREFETCH={1 if args.access == 'prefetch' else 0}",
        f"-DDUMMY_BUFFER_PAGES={args.dummy_buffer_pages}",
        f"-DNO_TRIGGER={1 if args.no_trigger else 0}",
        f"-DENABLE_SCHED_YIELD={1 if args.context_switch else 0}",
        "-o",
        OUT,
        SRC,
        UTIL_SRC,
    ]
    compile_cmd[1:1] = pmu_defines_for_arch(arch)
    timer_define = timer_define_for(arch)
    if timer_define is not None:
        compile_cmd.insert(-4, timer_define)
    if arch == "X925":
        compile_cmd[1:1] = arch_cflags_for(arch)
    return subprocess.run(compile_cmd).returncode


def run_binary(core):
    env = os.environ.copy()
    if args.pmu_device is not None:
        env["PMU_DEVICE"] = args.pmu_device
    return subprocess.run(
        ["taskset", "-c", str(core), OUT],
        capture_output=True,
        text=True,
        env=env,
    )


def print_pmu_output(output):
    """Print only PMU comment lines from the captured program output."""
    for line in output.splitlines():
        stripped = line.lstrip()
        if stripped.startswith("# PMU") or stripped.startswith("PMU:"):
            print(line)

def run_one(arch, core, train_step):
    threshold_ns = threshold_ns_for(arch)
    threshold_unit = timer_unit_for(arch)
    print("=" * 60)
    print(
        f"access={args.access}, arch={arch}, core={core}, "
        f"stride={args.stride} lines, train_step={train_step}, "
        f"predicted_position={predicted_position(train_step)}, "
        f"probe_mode={args.probe_mode}, "
        f"single_probe_position={single_probe_position(train_step)}, "
        f"rounds={args.rounds}, threshold={threshold_ns} {threshold_unit}, "
        f"timer={timer_for_arch(arch)}, "
        f"dummy_access={args.dummy_access}, "
        f"dummy_order={args.dummy_order}, "
        f"dummy_buffer_pages={args.dummy_buffer_pages}, "
        f"context_switch={args.context_switch}, "
    )

    if compile_test(train_step, arch) != 0:
        print("Compile failed")
        return []

    run = run_binary(core)
    if run.returncode != 0:
        print("Execution failed")
        if run.stdout:
            print(run.stdout)
        if run.stderr:
            print(run.stderr)
        return []

    print_pmu_output(run.stdout)
    print_pmu_output(run.stderr)

    raw_path = raw_path_for(arch, core, train_step)
    with open(raw_path, "w") as f:
        f.write(run.stdout)
    rows = parse_output(run.stdout, train_step, threshold_ns)
    if not rows:
        print("No results to save")
        return []

    write_tsv(rows, tsv_path_for(arch, core, train_step))
    return rows


def predicted_row(rows, train_step):
    pos = predicted_position(train_step)
    for row in rows:
        if row["position"] == pos:
            return row
    return None


def colored_yes_no(value):
    text = "yes" if value else "no"
    color = GREEN if value else RED
    return f"{color}{text}{RESET}"


def print_predicted_result(arch, core, train_step, rows):
    threshold_ns = threshold_ns_for(arch)
    threshold_unit = timer_unit_for(arch)
    row = predicted_row(rows, train_step)
    if row is None:
        print(
            f"{arch} core={core} train_step={train_step}: "
            f"predicted_position={predicted_position(train_step)} missing"
        )
        return

    prefetched = row["probes"] > 0 and row["avg_ns"] <= threshold_ns
    print(
        f"{arch} core={core} train_step={train_step}: "
        f"predicted_position={row['position']} "
        f"avg_{threshold_unit}={row['avg_ns']} "
        f"prefetched={colored_yes_no(prefetched)} "
        f"threshold={threshold_ns} {threshold_unit}"
    )


def plot_bar_chart(rows, arch, core, train_step):
    threshold_ns = threshold_ns_for(arch)
    threshold_unit = timer_unit_for(arch)
    try:
        import matplotlib.pyplot as plt
        from matplotlib.patches import Patch
    except ModuleNotFoundError as exc:
        print(f"Skipping bar plot: missing Python package '{exc.name}'.")
        print("Install plotting dependencies with:")
        print("  sudo apt install python3-matplotlib")
        print("or, for the current Python environment:")
        print("  python3 -m pip install matplotlib")
        return

    sorted_rows = sorted(rows, key=lambda row: row["position"])
    positions = [row["position"] for row in sorted_rows]
    values = [row["avg_ns"] for row in sorted_rows]
    colors = [
        ROLE_COLORS.get(row["role"], ROLE_COLORS["cache_miss"])
        for row in sorted_rows
    ]

    width = min(max(args.probe_positions / 5, 10), 18)
    fig, ax = plt.subplots(1, 1, figsize=(width, 4))

    bars = ax.bar(positions, values, color=colors, width=0.85,
                  edgecolor="black", linewidth=0.25)
    stride_bytes = args.stride * 64
    for bar, row in zip(bars, sorted_rows):
        if row["role"] != "accessed":
            continue
        access_step = row["offset_bytes"] // stride_bytes
        ax.annotate(
            str(access_step),
            xy=(bar.get_x() + bar.get_width() / 2, bar.get_height()),
            xytext=(0, 3),
            textcoords="offset points",
            ha="center",
            va="bottom",
            fontsize=8,
            color="black",
        )
    ax.axhline(threshold_ns, color="black",
               linestyle="--", linewidth=0.9)
    ax.axvline(predicted_position(train_step), color="#0072B2",
               linestyle=":", linewidth=1.0)

    ax.set_title(
        f"{args.access} stride, train-step={train_step}",
        loc="left",
        pad=4,
    )
    ax.set_ylabel(f"Average reload {threshold_unit}")
    ax.set_xlabel("Probe cache-line index")
    ax.set_ylim(0, max(300, max(values) * 1.05 if values else 300))
    ax.set_xlim(-1, max(positions) + 1 if positions else args.probe_positions)
    ax.grid(axis="y", alpha=0.25)

    tick_step = max(1, args.probe_positions // 16)
    ax.set_xticks(range(0, args.probe_positions, tick_step))

    legend_items = [
        Patch(facecolor=ROLE_COLORS["accessed"], edgecolor="black",
              label="accessed"),
        Patch(facecolor=ROLE_COLORS["prefetched"], edgecolor="black",
              label="prefetched"),
        Patch(facecolor=ROLE_COLORS["cache_miss"], edgecolor="black",
              label="cache_miss"),
    ]
    ax.legend(handles=legend_items, loc="upper right", frameon=False, ncol=3)

    fig.suptitle(
        f"{arch} core {core}, stride={args.stride}, "
        f"predicted_position={predicted_position(train_step)}, "
        f"threshold={threshold_ns} {threshold_unit}",
        x=0.01,
        ha="left",
    )
    fig.tight_layout(rect=(0, 0, 1, 0.94))
    path = plot_path_for(arch, core, train_step)
    fig.savefig(path, dpi=300)
    plt.close(fig)
    print(f"Saved bar chart to {path}")


def main():
    ensure_dirs()
    for arch, core in targets():
        for dummy_access, dummy_order, dummy_pages in dummy_configs():
            args.dummy_access = dummy_access
            args.dummy_order = dummy_order
            args.dummy_buffer_pages = dummy_pages
            for train_step in train_steps():
                if args.plot_only:
                    path = tsv_path_for(arch, core, train_step)
                    if not os.path.exists(path):
                        print(f"Error: TSV result '{path}' not found.")
                        continue
                    rows = read_tsv(path)
                else:
                    rows = run_one(arch, core, train_step)
                if rows:
                    print_predicted_result(arch, core, train_step, rows)
                    if not args.no_plot:
                        plot_bar_chart(rows, arch, core, train_step)


if __name__ == "__main__":
    main()
