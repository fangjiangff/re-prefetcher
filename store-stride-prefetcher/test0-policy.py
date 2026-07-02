import argparse
import csv
import os
import subprocess
import sys

from cross_test_config import ARCH_CONFIG, arch_choices


BASE_DIR = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.join(BASE_DIR, "test0-policy.c")
UTIL_SRC = os.path.join(BASE_DIR, "until.c")
OUT = os.path.join(BASE_DIR, "bin", "test0-policy")

DEFAULT_SEQUENCE = "0,24,11"
DEFAULT_ROUNDS = 40000
DEFAULT_PROBE_POSITIONS = 100
GREEN = "\033[32m"
RED = "\033[31m"
RESET = "\033[0m"

ROLE_COLORS = {
    "accessed": "#D55E00",
    "possible_prefetch": "#0072B2",
    "cache_miss": "#BDBDBD",
}


def parse_csv_list(value):
    return [item.strip() for item in value.split(",") if item.strip()]


def parse_sequence(value):
    try:
        sequence = [int(item) for item in parse_csv_list(value)]
    except ValueError:
        raise argparse.ArgumentTypeError("sequence must contain comma-separated integers")
    if not sequence:
        raise argparse.ArgumentTypeError("sequence must not be empty")
    if any(line < 0 for line in sequence):
        raise argparse.ArgumentTypeError("sequence line indices must be >= 0")
    return sequence


def sequence_name(sequence):
    return "-".join(str(line) for line in sequence)


def parse_type_sequence(value):
    compact = "".join(ch for ch in value.lower() if ch not in {",", " ", "\t"})
    if not compact:
        raise argparse.ArgumentTypeError("type sequence must not be empty")
    unknown = sorted(set(compact) - {"s", "l"})
    if unknown:
        raise argparse.ArgumentTypeError(
            "type sequence may only contain 's' for store and 'l' for load"
        )
    return list(compact)


def type_sequence_name(type_sequence):
    return "".join(type_sequence)


def access_label():
    if args.type is not None:
        return f"type{type_sequence_name(args.type)}"
    return args.access


def access_display_label():
    if args.type is not None:
        return f"type [{','.join(args.type)}]"
    return args.access


def access_type_at(index):
    if args.type is not None:
        return args.type[index]
    return args.access[0]


def parse_args():
    parser = argparse.ArgumentParser(
        description="Run a policy access sequence and report likely prefetched cache lines."
    )
    parser.add_argument("--arch", choices=arch_choices(), default=None,
                        help="Architecture label. If omitted, all configured architectures are tested.")
    parser.add_argument("--core", type=int, default=None,
                        help="Override CPU core used by taskset. Default is selected from --arch.")
    parser.add_argument("--arches", default=None,
                        help="Comma-separated architecture labels.")
    parser.add_argument("--cores", default=None,
                        help="Comma-separated CPU core overrides for --arches.")
    parser.add_argument("--sequence", type=parse_sequence,
                        default=parse_sequence(DEFAULT_SEQUENCE),
                        help=f"Comma-separated cache-line access sequence. Default: {DEFAULT_SEQUENCE}")
    parser.add_argument("--rounds", type=int, default=DEFAULT_ROUNDS,
                        help=f"Rounds per test. Default: {DEFAULT_ROUNDS}")
    parser.add_argument("--probe-positions", type=int,
                        default=DEFAULT_PROBE_POSITIONS,
                        help=f"Probe cache lines 0..N-1. Default: {DEFAULT_PROBE_POSITIONS}")
    parser.add_argument("--hit-threshold-ns", type=int, default=None,
                        help="Line is reported as possible-prefetched when avg latency <= this value. Default is selected from --arch.")
    parser.add_argument("--timer", choices=["gettime", "rdtsc"], default="gettime",
                        help="x86 timestamp source. Default: gettime")
    parser.add_argument("--access", choices=["store", "load", "prefetch"], default="store",
                        help="Instruction used for the whole policy sequence when --type is omitted. Default: store")
    parser.add_argument("--type", type=parse_type_sequence, default=None,
                        help=("Per-access operation sequence. Use s for store and l for load; "
                              "accepts forms like sls or s,l,s. Overrides --access."))
    parser.add_argument("--dummy-buffer-pages", type=int, default=10,
                        help="Number of pages in dummy_buffer. Default: 10")
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
    if args.rounds < 1:
        parser.error("--rounds must be >= 1")
    if args.probe_positions < 1:
        parser.error("--probe-positions must be >= 1")
    if max(args.sequence) >= args.probe_positions:
        parser.error("--probe-positions must be greater than every sequence line")
    if args.type is not None and len(args.type) != len(args.sequence):
        parser.error("--type length must match --sequence length")
    if args.type is not None and args.access != "store":
        parser.error("--type overrides --access; leave --access as store when using --type")
    if args.dummy_buffer_pages < 1:
        parser.error("--dummy-buffer-pages must be >= 1")
    if args.hit_threshold_ns is not None and args.hit_threshold_ns < 1:
        parser.error("--hit-threshold-ns must be >= 1")
    return args


args = parse_args()
result_dir = os.path.join(BASE_DIR, "res", "store-stride")
plot_dir = os.path.join(BASE_DIR, "res", "barplots")
raw_dir = os.path.join(result_dir, "raw")
dump_dir = os.path.join(result_dir, "dump")


def targets():
    if args.arch is not None:
        core = args.core if args.core is not None else ARCH_CONFIG[args.arch]["core"]
        return [(args.arch, core)]

    if args.arches is not None:
        arches = parse_csv_list(args.arches)
        unknown = [arch for arch in arches if arch not in ARCH_CONFIG]
        if unknown:
            print("Error: unknown architecture(s): " + ", ".join(unknown), file=sys.stderr)
            sys.exit(2)
        if args.cores is None:
            cores = [ARCH_CONFIG[arch]["core"] for arch in arches]
        else:
            try:
                cores = [int(core) for core in parse_csv_list(args.cores)]
            except ValueError:
                print("Error: --cores must contain comma-separated integers.", file=sys.stderr)
                sys.exit(2)
    else:
        arches = list(arch_choices())
        cores = [ARCH_CONFIG[arch]["core"] for arch in arches]

    if len(arches) != len(cores):
        print("Error: architecture and core lists must have the same length.", file=sys.stderr)
        sys.exit(2)
    return list(zip(arches, cores))


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


def threshold_for(arch):
    if args.hit_threshold_ns is not None:
        return args.hit_threshold_ns
    return ARCH_CONFIG[arch]["threshold_ns"]


def result_name(arch, core):
    timer_suffix = ""
    if is_x86_arch(arch) and args.timer != "gettime":
        timer_suffix = f"-timer{args.timer}"
    dummy_suffix = "" if args.dummy_buffer_pages == 10 else f"-dummypages{args.dummy_buffer_pages}"
    type_suffix = f"-{access_label()}"
    return (
        f"{arch}-core{core}-policy-seq{sequence_name(args.sequence)}"
        f"{type_suffix}{timer_suffix}{dummy_suffix}"
    )


def tsv_path_for(arch, core):
    return os.path.join(result_dir, f"{result_name(arch, core)}.tsv")


def raw_path_for(arch, core):
    return os.path.join(raw_dir, f"{result_name(arch, core)}.txt")


def dump_path_for(arch, core):
    return os.path.join(dump_dir, f"{result_name(arch, core)}.dump")


def plot_path_for(arch, core):
    return os.path.join(plot_dir, f"{result_name(arch, core)}.png")


def ensure_dirs():
    os.makedirs(os.path.dirname(OUT), exist_ok=True)
    os.makedirs(result_dir, exist_ok=True)
    os.makedirs(plot_dir, exist_ok=True)
    os.makedirs(raw_dir, exist_ok=True)
    os.makedirs(dump_dir, exist_ok=True)


def classify_position(position, avg_value, probes, threshold):
    if position in set(args.sequence):
        return "accessed"
    if probes > 0 and avg_value <= threshold:
        return "possible_prefetch"
    return "cache_miss"


def parse_output(output, arch):
    rows = []
    threshold = threshold_for(arch)
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
            avg_value = int(fields[2])
            probes = int(fields[3])
        except ValueError:
            print(f"Skipping non-numeric output row: {line}", file=sys.stderr)
            continue
        rows.append({
            "position": position,
            "offset_bytes": offset_bytes,
            "role": classify_position(position, avg_value, probes, threshold),
            "avg": avg_value,
            "probes": probes,
        })
    return rows


def write_tsv(rows, path):
    with open(path, "w", newline="") as f:
        writer = csv.DictWriter(
            f,
            fieldnames=["position", "offset_bytes", "role", "avg", "probes"],
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
                "avg": int(row["avg"]),
                "probes": int(row["probes"]),
            })
    return rows


def compile_test(arch):
    sequence_macro = ",".join(str(line) for line in args.sequence)
    type_macro = None
    if args.type is not None:
        type_macro = ",".join(f"'{item}'" for item in args.type)
    compile_cmd = [
        args.cc,
        "-std=gnu11",
        "-O0",
        "-static",
        f"-DROUNDS={args.rounds}",
        f"-DPROBE_POSITIONS={args.probe_positions}",
        f"-DACCESS_SEQUENCE={sequence_macro}",
        f"-DACCESS_SEQUENCE_LEN={len(args.sequence)}",
        f"-DTRAIN_ACCESS_LOAD={1 if args.access == 'load' else 0}",
        f"-DTRAIN_ACCESS_PREFETCH={1 if args.access == 'prefetch' else 0}",
        f"-DDUMMY_BUFFER_PAGES={args.dummy_buffer_pages}",
        "-o",
        OUT,
        SRC,
        UTIL_SRC,
    ]
    if type_macro is not None:
        compile_cmd.insert(-4, f"-DACCESS_TYPE_SEQUENCE={type_macro}")
    timer_define = timer_define_for(arch)
    if timer_define is not None:
        compile_cmd.insert(-4, timer_define)
    return subprocess.run(compile_cmd).returncode


def write_dump(arch, core):
    if args.no_dump:
        return
    path = dump_path_for(arch, core)
    run = subprocess.run([args.objdump, "-d", OUT], capture_output=True, text=True)
    if run.returncode != 0:
        print(f"Warning: failed to generate dump file '{path}'")
        if run.stderr:
            print(run.stderr, file=sys.stderr)
        return
    with open(path, "w") as f:
        f.write(run.stdout)
    print(f"Saved dump to {path}")


def run_binary(core):
    return subprocess.run(["taskset", "-c", str(core), OUT], capture_output=True, text=True)


def possible_prefetch_rows(rows):
    return [row for row in rows if row["role"] == "possible_prefetch"]


def possible_strides_for(sequence):
    strides = []
    seen = set()
    for current_index in range(1, len(sequence)):
        current = sequence[current_index]
        for previous in reversed(sequence[:current_index]):
            stride = abs(current - previous)
            if stride == 0 or stride in seen:
                continue
            seen.add(stride)
            strides.append(stride)
    return strides


def prefetch_explanations(position, sequence, strides):
    explanations = []
    seen = set()

    for base in sequence:
        distance = position - base
        if distance <= 0:
            continue
        for stride in strides:
            if distance % stride != 0:
                continue
            multiplier = distance // stride
            explanation = (base, stride, multiplier)
            if explanation in seen:
                continue
            seen.add(explanation)
            explanations.append(explanation)

    if not explanations:
        return []

    min_stride = min(stride for _, stride, _ in explanations)
    smallest_stride = [
        explanation for explanation in explanations
        if explanation[1] == min_stride
    ]
    min_multiplier = min(multiplier for _, _, multiplier in smallest_stride)
    return [
        explanation for explanation in smallest_stride
        if explanation[2] == min_multiplier
    ]

def format_explanation(base, stride, multiplier):
    if multiplier == 1:
        return f"{base}+{stride}"
    return f"{base}+{multiplier}*{stride}"


def prefetch_expression(position, sequence, strides):
    explanations = prefetch_explanations(position, sequence, strides)
    if not explanations:
        return "unexplained"
    return " | ".join(
        format_explanation(base, stride, multiplier)
        for base, stride, multiplier in explanations
    )


def format_prefetch_row(row, sequence, strides):
    expression = prefetch_expression(row["position"], sequence, strides)
    return f"{row['position']}({expression})"


def print_summary(arch, core, rows):
    threshold = threshold_for(arch)
    unit = timer_unit_for(arch)
    possible = possible_prefetch_rows(rows)
    strides = possible_strides_for(args.sequence)

    print(
        f"{arch} core={core} access={access_display_label()} sequence={args.sequence} "
        f"threshold={threshold} {unit} possible_prefetch:"
    )
    print(f"possible_strides: {strides}")
    if not possible:
        print("none")
        return

    for row in possible:
        print(format_prefetch_row(row, args.sequence, strides))


def run_one(arch, core):
    unit = timer_unit_for(arch)
    threshold = threshold_for(arch)
    print("=" * 60)
    print(
        f"access={access_display_label()}, arch={arch}, core={core}, "
        f"sequence={args.sequence}, rounds={args.rounds}, "
        f"probe_positions={args.probe_positions}, "
        f"threshold={threshold} {unit}, "
        f"timer={args.timer if is_x86_arch(arch) else 'arch-default'}"
    )
    if compile_test(arch) != 0:
        print("Compile failed")
        return []
    write_dump(arch, core)
    run = run_binary(core)
    if run.returncode != 0:
        print("Execution failed")
        if run.stdout:
            print(run.stdout)
        if run.stderr:
            print(run.stderr, file=sys.stderr)
        return []
    with open(raw_path_for(arch, core), "w") as f:
        f.write(run.stdout)
    rows = parse_output(run.stdout, arch)
    if rows:
        write_tsv(rows, tsv_path_for(arch, core))
    return rows


def plot_bar_chart(rows, arch, core):
    try:
        import matplotlib.pyplot as plt
        from matplotlib.patches import Patch
    except ModuleNotFoundError as exc:
        print(f"Skipping bar plot: missing Python package '{exc.name}'.")
        return

    threshold = threshold_for(arch)
    unit = timer_unit_for(arch)
    sorted_rows = sorted(rows, key=lambda row: row["position"])
    positions = [row["position"] for row in sorted_rows]
    values = [row["avg"] for row in sorted_rows]
    colors = [ROLE_COLORS.get(row["role"], ROLE_COLORS["cache_miss"]) for row in sorted_rows]

    width = min(max(args.probe_positions / 5, 10), 20)
    fig, ax = plt.subplots(1, 1, figsize=(width, 4))
    ax.bar(positions, values, color=colors, width=0.85,
           edgecolor="black", linewidth=0.25)
    y_limit = max(300, max(values) * 1.18 if values else 300)
    strides = possible_strides_for(args.sequence)
    ax.axhline(threshold, color="black", linestyle="--", linewidth=0.9)
    access_labels = {}
    for index, line in enumerate(args.sequence):
        access_labels.setdefault(line, []).append(
            f"{index + 1}:{access_type_at(index)}"
        )
    for line in args.sequence:
        ax.axvline(line, color="#D55E00", linestyle=":", linewidth=0.8)
    for line, labels in access_labels.items():
        ax.text(
            line,
            y_limit * 0.97,
            ",".join(labels),
            ha="center",
            va="top",
            rotation=90,
            fontsize=20,
            color="#7A2E00",
            bbox={"facecolor": "white", "edgecolor": "none", "alpha": 0.8, "pad": 1.5},
        )
    for row in sorted_rows:
        if row["role"] != "possible_prefetch":
            continue
        ax.text(
            row["position"],
            min(row["avg"] + y_limit * 0.035, y_limit * 0.78),
            prefetch_expression(row["position"], args.sequence, strides),
            ha="center",
            va="bottom",
            rotation=90,
            fontsize=16,
            color="#00517A",
            bbox={"facecolor": "white", "edgecolor": "none", "alpha": 0.75, "pad": 1.0},
        )
    ax.set_title(
        f"{access_display_label()} sequence {args.sequence}",
        loc="center",
        pad=10,
        fontsize=22,
        fontweight="bold",
    )
    ax.set_ylabel(f"Average latency", fontsize=18)
    ax.set_xlabel("Probe cache-line index", fontsize=18)
    ax.set_ylim(0, y_limit)
    ax.set_xlim(-1, max(positions) + 1 if positions else args.probe_positions)
    ax.grid(axis="y", alpha=0.25)
    ax.set_xticks([0, 5, 10])
    ax.tick_params(axis="both", labelsize=14)
    legend_items = [
        Patch(facecolor=ROLE_COLORS["accessed"], edgecolor="black", label="accessed"),
        Patch(facecolor=ROLE_COLORS["possible_prefetch"], edgecolor="black", label="possible_prefetch"),
        Patch(facecolor=ROLE_COLORS["cache_miss"], edgecolor="black", label="cache_miss"),
    ]
    ax.legend(
        handles=legend_items,
        loc="upper right",
        frameon=False,
        ncol=3,
        fontsize=14,
        handlelength=1.6,
        handleheight=1.0,
        columnspacing=1.2,
    )
    fig.tight_layout()
    path = plot_path_for(arch, core)
    fig.savefig(path, dpi=300)
    plt.close(fig)
    print(f"Saved bar chart to {path}")


def main():
    ensure_dirs()
    for arch, core in targets():
        if args.plot_only:
            path = tsv_path_for(arch, core)
            if not os.path.exists(path):
                print(f"Error: TSV result '{path}' not found.")
                continue
            rows = read_tsv(path)
        else:
            rows = run_one(arch, core)
        if rows:
            print_summary(arch, core, rows)
            if not args.no_plot:
                plot_bar_chart(rows, arch, core)


if __name__ == "__main__":
    main()
