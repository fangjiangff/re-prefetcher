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
SRC = os.path.join(BASE_DIR, "test0-exist2.c")
UTIL_SRC = os.path.join(ROOT_DIR, "until.c")
OUT = os.path.join(ROOT_DIR, "bin", "test0-exist2")

DEFAULT_STRIDE_LINES = 5
DEFAULT_TRAIN_STEP = 3
DEFAULT_TRAIN_STEP_MIN = 1
DEFAULT_TRAIN_STEP_MAX = 10
DEFAULT_ROUNDS = 40000
DEFAULT_PROBE_POSITIONS = 100
DEFAULT_ARRAY2_PAGES = 160
DEFAULT_RANDOM_ACTIVITY_ITERS = 1
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
    parser.add_argument("--batch", action="store_true",
                        help="Sweep train-step-min..train-step-max instead of a single train-step.")
    parser.add_argument("--train-step-min", type=int, default=DEFAULT_TRAIN_STEP_MIN,
                        help=f"First train-step for --batch. Default: {DEFAULT_TRAIN_STEP_MIN}")
    parser.add_argument("--train-step-max", type=int, default=DEFAULT_TRAIN_STEP_MAX,
                        help=f"Last train-step for --batch. Default: {DEFAULT_TRAIN_STEP_MAX}")
    parser.add_argument("--rounds", type=int, default=DEFAULT_ROUNDS,
                        help=f"Rounds per test. Default: {DEFAULT_ROUNDS}")
    parser.add_argument("--probe-positions", type=int,
                        default=DEFAULT_PROBE_POSITIONS,
                        help=f"Positions to probe. Default: {DEFAULT_PROBE_POSITIONS}")
    parser.add_argument("--array2-pages", type=int, default=DEFAULT_ARRAY2_PAGES,
                        help=f"array2 mapping size in pages. Default: {DEFAULT_ARRAY2_PAGES}")
    parser.add_argument("--hit-threshold-ns", type=int,
                        default=None,
                        help="Predicted line is treated as prefetched when "
                             "avg_ns <= this value. Default is selected from --arch.")
    parser.add_argument("--timer", choices=["gettime", "rdtsc"],
                        default="gettime",
                        help=("x86 timestamp source. gettime uses "
                              "clock_gettime; rdtsc uses rdtscp/rdtsc. "
                              "Default: gettime"))
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
    parser.add_argument("--dummy-buffer-pages", type=int, default=10,
                        help="Number of pages in dummy_buffer. Default: 10")
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
                        help="Call sched_yield between training and trigger.")
    parser.add_argument("--context-switch-yields", type=int, default=1,
                        help="Number of sched_yield calls between training and trigger.")
    parser.add_argument("--random-activity-iters", type=int,
                        default=DEFAULT_RANDOM_ACTIVITY_ITERS,
                        help=("Iterations for FetchBench-style random_activity. "
                              f"Default: {DEFAULT_RANDOM_ACTIVITY_ITERS}"))
    parser.add_argument("--cc", default=os.environ.get("CC", "gcc"),
                        help="Compiler command. Default: $CC or gcc")
    parser.add_argument("--objdump", default=os.environ.get("OBJDUMP", "objdump"),
                        help="Objdump command used to generate .dump files. Default: $OBJDUMP or objdump")
    parser.add_argument("--no-dump", action="store_true",
                        help="Do not generate objdump disassembly files.")
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
    if args.train_step < 1:
        parser.error("--train-step must be >= 1")
    if args.train_step_min < 1:
        parser.error("--train-step-min must be >= 1")
    if args.train_step_max < args.train_step_min:
        parser.error("--train-step-max must be >= --train-step-min")
    if args.rounds < 1:
        parser.error("--rounds must be >= 1")
    if args.probe_positions < 1:
        parser.error("--probe-positions must be >= 1")
    if args.array2_pages < 1:
        parser.error("--array2-pages must be >= 1")
    if args.dummy_buffer_pages < 1:
        parser.error("--dummy-buffer-pages must be >= 1")
    if args.hit_threshold_ns is not None and args.hit_threshold_ns < 1:
        parser.error("--hit-threshold-ns must be >= 1")
    if args.context_switch_yields < 1:
        parser.error("--context-switch-yields must be >= 1")
    if args.random_activity_iters < 1:
        parser.error("--random-activity-iters must be >= 1")
    max_train_step = args.train_step_max if args.batch else args.train_step
    if max_train_step * args.stride >= args.probe_positions:
        parser.error("predicted position train_step * stride must be inside probe positions")
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
dump_dir = os.path.join(result_dir, "dump")


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
    if args.batch:
        return range(args.train_step_min, args.train_step_max + 1)
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


def timer_unit_for(arch):
    if is_x86_arch(arch) and args.timer == "rdtsc":
        return "cycles"
    return "ns"


def timer_define_for(arch):
    if not is_x86_arch(arch):
        return None
    if args.timer == "rdtsc":
        return "-DRDTSC=1"
    return "-DGETTIME=1"


def micro_arch_name(arch, core, train_step):
    trigger_suffix = "-no-trigger" if args.no_trigger else ""
    switch_suffix = (
        f"-ctxswitch{args.context_switch_yields}"
        if args.context_switch else ""
    )
    dummy_suffix = ""
    mapping_suffix = ""
    timer_suffix = ""
    if is_x86_arch(arch) and args.timer != "gettime":
        timer_suffix = f"-timer{args.timer}"
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
    if args.array2_pages != DEFAULT_ARRAY2_PAGES:
        mapping_suffix += f"-pages{args.array2_pages}"
    if args.random_activity_iters != DEFAULT_RANDOM_ACTIVITY_ITERS:
        mapping_suffix += f"-randact{args.random_activity_iters}"
    return (
        f"{arch}-core{core}-stride{args.stride}"
        f"-train{train_step}-{args.access}-mapping"
        f"{timer_suffix}{trigger_suffix}{switch_suffix}"
        f"{dummy_suffix}{mapping_suffix}"
    )


def tsv_path_for(arch, core, train_step):
    return os.path.join(result_dir, f"{micro_arch_name(arch, core, train_step)}.tsv")


def raw_path_for(arch, core, train_step):
    return os.path.join(raw_dir, f"{micro_arch_name(arch, core, train_step)}.txt")


def dump_path_for(arch, core, train_step):
    return os.path.join(dump_dir, f"{micro_arch_name(arch, core, train_step)}.dump")


def plot_path_for(arch, core, train_step):
    return os.path.join(plot_dir, f"{micro_arch_name(arch, core, train_step)}.png")


def ensure_dirs():
    os.makedirs(os.path.dirname(OUT), exist_ok=True)
    os.makedirs(result_dir, exist_ok=True)
    os.makedirs(plot_dir, exist_ok=True)
    os.makedirs(raw_dir, exist_ok=True)
    os.makedirs(dump_dir, exist_ok=True)


def accessed_offsets(train_step):
    stride_bytes = args.stride * 64
    accessed_steps = train_step - 1 if args.no_trigger else train_step
    return {step * stride_bytes for step in range(accessed_steps)}


def predicted_position(train_step):
    return train_step * args.stride


def predicted_offset(train_step):
    return predicted_position(train_step) * 64


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
        args.cc,
        "-std=gnu11",
        "-O0",
        "-static",
        "-march=armv8.5-a+predres",
        f"-DSTRIDE_BYTES={args.stride * 64}",
        f"-DTRAIN_STEP={train_step}",
        f"-DROUNDS={args.rounds}",
        f"-DPROBE_POSITIONS={args.probe_positions}",
        f"-DARRAY2_PAGES={args.array2_pages}",
        f"-DCPU_ID=0",
        f"-DTRAIN_ACCESS_LOAD={1 if args.access == 'load' else 0}",
        f"-DTRAIN_ACCESS_PREFETCH={1 if args.access == 'prefetch' else 0}",
        f"-DDUMMY_BUFFER_PAGES={args.dummy_buffer_pages}",
        f"-DDUMMY_ACCESS_LOAD={1 if args.dummy_access == 'load' else 0}",
        f"-DDUMMY_ACCESS_STORE={1 if args.dummy_access == 'store' else 0}",
        f"-DDUMMY_ACCESS_PERMUTED={1 if args.dummy_order == 'permuted' else 0}",
        f"-DNO_TRIGGER={1 if args.no_trigger else 0}",
        f"-DCONTEXT_SWITCH_BEFORE_TRIGGER={1 if args.context_switch else 0}",
        f"-DCONTEXT_SWITCH_YIELDS={args.context_switch_yields}",
        f"-DRANDOM_ACTIVITY_ITERS={args.random_activity_iters}",
        "-o",
        OUT,
        SRC,
        UTIL_SRC,
    ]
    timer_define = timer_define_for(arch)
    if timer_define is not None:
        compile_cmd.insert(-4, timer_define)
    return subprocess.run(compile_cmd).returncode


def write_dump(arch, core, train_step):
    if args.no_dump:
        return

    path = dump_path_for(arch, core, train_step)
    run = subprocess.run(
        [args.objdump, "-d", OUT],
        capture_output=True,
        text=True,
    )
    if run.returncode != 0:
        print(f"Warning: failed to generate dump file '{path}'")
        if run.stderr:
            print(run.stderr, file=sys.stderr)
        return

    with open(path, "w") as f:
        f.write(run.stdout)
    print(f"Saved dump to {path}")


def run_binary(core):
    return subprocess.run(
        ["taskset", "-c", str(core), OUT],
        capture_output=True,
        text=True,
    )


def run_one(arch, core, train_step):
    threshold_ns = threshold_ns_for(arch)
    threshold_unit = timer_unit_for(arch)
    print("=" * 60)
    print(
        f"access={args.access}, arch={arch}, core={core}, "
        f"stride={args.stride} lines, train_step={train_step}, "
        f"predicted_position={predicted_position(train_step)}, "
        f"rounds={args.rounds}, threshold={threshold_ns} {threshold_unit}, "
        f"array2_pages={args.array2_pages}, "
        f"timer={args.timer if is_x86_arch(arch) else 'arch-default'}, "
        f"dummy_access={args.dummy_access}, "
        f"dummy_order={args.dummy_order}, "
        f"dummy_buffer_pages={args.dummy_buffer_pages}, "
        f"context_switch={args.context_switch}, "
        f"context_switch_yields={args.context_switch_yields}, "
        f"random_activity_iters={args.random_activity_iters}"
    )

    if compile_test(train_step, arch) != 0:
        print("Compile failed")
        return []
    write_dump(arch, core, train_step)

    run = run_binary(core)
    if run.returncode != 0:
        print("Execution failed")
        if run.stdout:
            print(run.stdout)
        if run.stderr:
            print(run.stderr)
        return []

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

    ax.bar(positions, values, color=colors, width=0.85,
           edgecolor="black", linewidth=0.25)
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
