import argparse
import csv
import os
import re
import subprocess
import sys

os.environ.setdefault("MPLCONFIGDIR", "/tmp/matplotlib-cache")
import matplotlib.pyplot as plt

ROOT_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
if ROOT_DIR not in sys.path:
    sys.path.insert(0, ROOT_DIR)

from cross_test_config import (
    apply_access_defaults,
    apply_single_core_defaults,
    apply_threshold_defaults,
    arch_choices,
    default_timer_for_arch,
    timer_define_for_arch,
)


BASE_DIR = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.join(BASE_DIR, "test1-index-mem-va.c")
UTIL_SRC = os.path.join(ROOT_DIR, "until.c")
OUT = os.path.join(ROOT_DIR, "bin", "test1-index-mem-va")

DEFAULT_VA1_BASE = "0x600000000"
DEFAULT_STRIDE_LINES = 5
DEFAULT_MIN_DIFF_BIT = 12
DEFAULT_MAX_DIFF_BIT = 47
DEFAULT_ROUNDS = 400
DEFAULT_PROBE_POSITIONS = 64
RESULT_DIR = os.path.join(ROOT_DIR, "res", "index-mem-va")
PLOT_DIR = os.path.join(ROOT_DIR, "res", "barplots")
PMU_PLOT_DIR = os.path.join(ROOT_DIR, "res", "lineplots")
PMU_RE = re.compile(
    r"^# PMU\s+(\S+)\s+.*\bper_round=([0-9]+(?:\.[0-9]+)?)"
)


def compile_test(args):
    os.makedirs(os.path.dirname(OUT), exist_ok=True)
    compile_cmd = [
        args.cc,
        "-std=gnu11",
        "-O0",
        "-static",
        f"-DARCH_NAME=\"{args.arch}\"",
        f"-DVA1_BASE={args.va1_base}",
        f"-DSTRIDE_LINES={args.stride}",
        f"-DSTORE_ACCESSES={args.accesses}",
        f"-DMIN_DIFF_BIT={args.min_diff_bit}",
        f"-DMAX_DIFF_BIT={args.max_diff_bit}",
        f"-DROUNDS={args.rounds}",
        f"-DPROBE_POSITIONS={args.probe_positions}",
        f"-DSTRIDE_ACCESS_LOAD={1 if args.access == 'load' else 0}",
        f"-DENABLE_CPP_RCTX={1 if args.arch == 'X925' else 0}",
        f"-DENABLE_PMU={args.enable_pmu}",
        f"-DENABLE_PROBE={args.enable_probe}",
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
    if args.arch == "X925":
        compile_cmd.insert(5, "-march=armv8.5-a+predres")
    return subprocess.run(compile_cmd)


def run_test(args):
    run_cmd = ["taskset", "-c", str(args.core), OUT]
    env = os.environ.copy()
    if args.pmu_device:
        env["PMU_DEVICE"] = args.pmu_device
    return subprocess.run(
        run_cmd, capture_output=True, text=True, env=env
    )


def sanitize_token(value):
    return str(value).replace("/", "_").replace(":", "_").replace("x", "x")


def micro_arch_name(args):
    va1 = sanitize_token(args.va1_base)
    return (
        f"{args.arch}-core{args.core}-index-mem-va"
        f"-stride{args.stride}-accesses{args.accesses}-{args.access}"
        f"-probe{args.probe_positions}"
        f"-pmu{args.enable_pmu}-probe-enabled{args.enable_probe}"
        f"-bits{args.min_diff_bit}-{args.max_diff_bit}"
        f"-va1{va1}"
    )


def output_path(args):
    if args.output:
        return args.output
    return os.path.join(RESULT_DIR, f"{micro_arch_name(args)}.tsv")


def raw_path(args):
    if args.raw_output:
        return args.raw_output
    return os.path.join(RESULT_DIR, "raw", f"{micro_arch_name(args)}.txt")


def plot_path(args):
    if args.plot_output:
        return args.plot_output
    return os.path.join(PLOT_DIR, f"{micro_arch_name(args)}.png")


def pmu_plot_path(args):
    if args.pmu_plot_output:
        return args.pmu_plot_output
    return os.path.join(
        PMU_PLOT_DIR, f"{micro_arch_name(args)}-pmu-counters.png"
    )


def ensure_parent(path):
    parent = os.path.dirname(path)
    if parent:
        os.makedirs(parent, exist_ok=True)


def parse_result(output):
    rows = []
    rows_by_case = {}
    current_pmu_case = None

    for line in output.splitlines():
        stripped = line.strip()
        if not stripped:
            continue
        if stripped.startswith("# PMU_CASE "):
            current_pmu_case = stripped.split(None, 2)[2]
            continue
        match = PMU_RE.match(stripped)
        if match:
            if current_pmu_case not in rows_by_case:
                raise ValueError(
                    f"PMU counter appears before result for {current_pmu_case}"
                )
            rows_by_case[current_pmu_case]["pmu"][match.group(1)] = float(
                match.group(2)
            )
            continue
        if stripped.startswith("#"):
            continue

        parts = stripped.split()
        if not parts or parts[0] == "case":
            continue
        if len(parts) != 2:
            raise ValueError(f"unexpected result row: {line}")

        row = {
            "case": parts[0],
            "latency_ns": int(parts[1]),
            "pmu": {},
        }
        rows.append(row)
        rows_by_case[row["case"]] = row

    if not rows:
        raise ValueError("no result data found")
    return rows


def classify_rows(rows, threshold_ns):
    for row in rows:
        latency = row["latency_ns"]
        row["prefetched"] = (
            "no" if latency < 0
            else "yes" if latency <= threshold_ns
            else "no"
        )
        row["threshold_ns"] = threshold_ns


def write_tsv(path, rows):
    counter_names = sorted({name for row in rows for name in row["pmu"]})
    fieldnames = [
        "case",
        "latency_ns",
        "prefetched",
        "threshold_ns",
    ] + counter_names
    ensure_parent(path)
    with open(path, "w", newline="") as f:
        writer = csv.DictWriter(f, fieldnames=fieldnames, delimiter="\t")
        writer.writeheader()
        for row in rows:
            flat = {
                "case": row["case"],
                "latency_ns": row["latency_ns"],
                "prefetched": row["prefetched"],
                "threshold_ns": row["threshold_ns"],
            }
            flat.update(row["pmu"])
            writer.writerow(flat)


def plot_result(path, rows, title, threshold_ns, vmin=None, vmax=None):
    if not path:
        return

    ensure_parent(path)
    labels = [row["case"] for row in rows]
    values = [max(0, row["latency_ns"]) for row in rows]
    colors = [
        "#0072B2" if row["prefetched"] == "yes" else "#BDBDBD"
        for row in rows
    ]
    if vmin is None:
        vmin = 0
    if vmax is None:
        vmax = max(max(values) if values else 0, threshold_ns) * 1.12

    width = max(10.5, 0.28 * len(rows) + 3.0)
    fig, ax = plt.subplots(figsize=(width, 4.8))
    ax.bar(range(len(rows)), values, color=colors, edgecolor="black",
           linewidth=0.25)
    ax.axhline(threshold_ns, color="black", linestyle="--", linewidth=1.0)
    ax.set_title(title, loc="left", pad=8)
    ax.set_xlabel("flipped VA bit")
    ax.set_ylabel("latency (ns)")
    ax.set_xticks(range(len(rows)))
    ax.set_xticklabels(labels, rotation=60, ha="right", fontsize=8)
    ax.set_ylim(vmin, vmax)
    ax.grid(axis="y", alpha=0.25)

    fig.tight_layout()
    fig.savefig(path, dpi=300)
    plt.close(fig)


def plot_pmu_bit_relation(path, rows, args):
    bit_rows = [row for row in rows if row["case"].startswith("bit_")]
    bit_rows.sort(key=lambda row: int(row["case"].split("_", 2)[1]))
    if not bit_rows:
        raise ValueError("no bit_N rows available for PMU plotting")

    bits = [int(row["case"].split("_", 2)[1]) for row in bit_rows]
    counter_names = sorted({name for row in bit_rows for name in row["pmu"]})
    if not counter_names:
        raise ValueError("no PMU counters available for plotting")
    for row in bit_rows:
        missing = [name for name in counter_names if name not in row["pmu"]]
        if missing:
            raise ValueError(
                f"{row['case']} is missing PMU counters: " + ", ".join(missing)
            )

    colors = ("#0072B2", "#D55E00", "#009E73", "#CC79A7")
    markers = ("o", "s", "^", "D")
    width = max(9.0, 0.22 * len(bits) + 3.0)
    fig, ax = plt.subplots(figsize=(width, 4.8))
    for index, counter_name in enumerate(counter_names):
        ax.plot(
            bits,
            [row["pmu"][counter_name] for row in bit_rows],
            color=colors[index % len(colors)],
            marker=markers[index % len(markers)],
            linewidth=1.5,
            markersize=4,
            label=counter_name,
        )
    ax.set_title(
        f"{args.arch} core {args.core}: {args.access} flipped VA bit "
        "vs PMU counters",
        loc="left",
    )
    ax.set_xlabel("flipped VA bit")
    ax.set_ylabel("PMU events / round")
    ax.set_xticks(bits)
    ax.grid(alpha=0.25)
    ax.legend()
    fig.tight_layout()
    ensure_parent(path)
    fig.savefig(path, dpi=300)
    plt.close(fig)


def parse_int_literal(value):
    try:
        return int(value, 0)
    except ValueError as exc:
        raise argparse.ArgumentTypeError(str(exc))


def main():
    parser = argparse.ArgumentParser(
        description="Test whether VA bits contribute to store-stride prefetcher indexing."
    )
    parser.add_argument("--arch", required=True, choices=arch_choices())
    parser.add_argument("--core", type=int, default=None,
                        help="Override CPU core. Default comes from --arch.")
    parser.add_argument("--va1-base", default=DEFAULT_VA1_BASE,
                        help=f"Page-aligned VA1 base. Default: {DEFAULT_VA1_BASE}")
    parser.add_argument("--stride", type=int, default=DEFAULT_STRIDE_LINES,
                        help="Stride in cache lines. Default: 5.")
    parser.add_argument("--access", choices=["store", "load"], default="store")
    parser.add_argument("--accesses", type=int, default=None,
                        help="Total train+trigger accesses. Default is selected "
                             "from --arch and --access.")
    parser.add_argument("--min-diff-bit", type=int, default=None,
                        help="First flipped VA bit. Default is the smallest "
                             "non-overlapping bit, at least 12.")
    parser.add_argument("--max-diff-bit", type=int, default=DEFAULT_MAX_DIFF_BIT)
    parser.add_argument("--rounds", type=int, default=DEFAULT_ROUNDS)
    parser.add_argument("--probe-positions", type=int,
                        default=DEFAULT_PROBE_POSITIONS,
                        help=f"Number of cache-line positions to probe. Default: {DEFAULT_PROBE_POSITIONS}")
    parser.add_argument("--enable-pmu", type=int, choices=[0, 1], default=0,
                        help="Compile with -DENABLE_PMU. Default: 0")
    parser.add_argument("--enable-probe", type=int, choices=[0, 1], default=None,
                        help="Compile with -DENABLE_PROBE. Default: 0 when "
                             "PMU is enabled, otherwise 1.")
    parser.add_argument("--pmu-device", default=None,
                        help="Override PMU_DEVICE for pmu.h raw-event fallback.")
    parser.add_argument("--threshold-ns", type=int, default=None,
                        help="Latency threshold for prefetched=yes. Default comes from --arch.")
    parser.add_argument("--timer", choices=["gettime", "rdtsc", "cntvct", "pmccntr"],
                        default=None,
                        help="Timestamp source. Default comes from cross_test_config.py.")
    parser.add_argument("--cc", default=os.environ.get("CC", "gcc"))
    parser.add_argument("--output", default=None)
    parser.add_argument("--raw-output", default=None)
    parser.add_argument("--plot-output", default=None)
    parser.add_argument("--pmu-plot-output", default=None)
    parser.add_argument("--plot-vmin", type=float, default=None)
    parser.add_argument("--plot-vmax", type=float, default=200)
    parser.add_argument("--no-plot", action="store_true")
    parser.add_argument("--no-compile", action="store_true")
    args = parser.parse_args()
    if args.enable_probe is None:
        args.enable_probe = 0 if args.enable_pmu else 1

    apply_single_core_defaults(args)
    apply_access_defaults(args)
    apply_threshold_defaults(args)
    if args.timer is None:
        args.timer = default_timer_for_arch(args.arch)

    max_access_pos = (
        args.accesses * args.stride
        if args.enable_probe
        else (args.accesses - 1) * args.stride
    )
    alias_required_bytes = (max_access_pos + 1) * 64
    alias_size = ((alias_required_bytes + 4095) // 4096) * 4096
    minimum_non_overlapping_bit = max(12, (alias_size - 1).bit_length())
    if args.min_diff_bit is None:
        args.min_diff_bit = minimum_non_overlapping_bit

    va1_base = parse_int_literal(args.va1_base)
    if args.core < 0:
        parser.error("--core must be >= 0")
    if va1_base % 4096 != 0:
        parser.error("--va1-base must be 4 KiB aligned")
    if args.stride < 1:
        parser.error("--stride must be >= 1")
    if args.accesses < 2:
        parser.error("--accesses must be >= 2")
    if args.min_diff_bit < minimum_non_overlapping_bit:
        parser.error(
            f"--min-diff-bit must be >= {minimum_non_overlapping_bit} "
            f"for alias_size={alias_size}"
        )
    if args.max_diff_bit < args.min_diff_bit or args.max_diff_bit >= 48:
        parser.error("--max-diff-bit must be in [min_diff_bit, 47]")
    if args.rounds < 1:
        parser.error("--rounds must be >= 1")
    if args.probe_positions < 1:
        parser.error("--probe-positions must be >= 1")
    predicted_pos = args.accesses * args.stride
    trigger_pos = (args.accesses - 1) * args.stride
    if (trigger_pos + 1) * 64 > alias_size:
        parser.error("trigger position must fit inside the computed alias mapping")
    if args.enable_probe and predicted_pos >= args.probe_positions:
        parser.error("predicted position accesses * stride must be inside probe positions")
    if args.enable_probe and (predicted_pos + 1) * 64 > 4096:
        parser.error("predicted position must fit inside one 4 KiB alias page")
    if args.threshold_ns < 1:
        parser.error("--threshold-ns must be >= 1")

    print(
        f"arch={args.arch}, core={args.core}, access={args.access}, "
        f"va1_base={args.va1_base}, stride={args.stride}, "
        f"accesses={args.accesses}, alias_size={alias_size}, "
        f"train_pos=0..{trigger_pos - args.stride}, "
        f"trigger_pos={trigger_pos}, predicted_pos={predicted_pos}, "
        f"rounds={args.rounds}, enable_pmu={args.enable_pmu}, "
        f"enable_probe={args.enable_probe}, "
        f"probe_positions={args.probe_positions}, threshold={args.threshold_ns} ns"
    )

    if not args.no_compile:
        res = compile_test(args)
        if res.returncode != 0:
            print("Compile failed", file=sys.stderr)
            return res.returncode

    run = run_test(args)
    if run.stdout:
        for line in run.stdout.splitlines():
            if not line.startswith("# probe_detail"):
                print(line)
    if run.stderr:
        print(run.stderr, end="", file=sys.stderr)
    if run.returncode != 0:
        print("Execution failed", file=sys.stderr)
        return run.returncode

    raw_result_path = raw_path(args)
    ensure_parent(raw_result_path)
    with open(raw_result_path, "w") as f:
        f.write(run.stdout)

    try:
        rows = parse_result(run.stdout)
    except ValueError as exc:
        print(f"Parse failed: {exc}", file=sys.stderr)
        return 1

    classify_rows(rows, args.threshold_ns)

    tsv_result_path = output_path(args)
    write_tsv(tsv_result_path, rows)
    print(f"Saved raw output to {raw_result_path}")
    print(f"Saved TSV to {tsv_result_path}")

    if not args.no_plot and args.enable_pmu:
        try:
            plot_pmu_bit_relation(pmu_plot_path(args), rows, args)
        except ValueError as exc:
            print(f"PMU plot failed: {exc}", file=sys.stderr)
            return 1
        print(f"Saved PMU line plot to {pmu_plot_path(args)}")

    if not args.no_plot and args.enable_probe:
        plot_title = (
            f"{args.arch} core {args.core}: {args.access} VA index contribution "
            f"(stride={args.stride}, accesses={args.accesses}, "
            f"VA1={args.va1_base})"
        )
        plot_result(
            plot_path(args),
            rows,
            plot_title,
            args.threshold_ns,
            vmin=args.plot_vmin,
            vmax=args.plot_vmax,
        )
        print(f"Saved plot to {plot_path(args)}")
    elif not args.no_plot:
        print("Skipped latency plot because ENABLE_PROBE=0")
    return 0


if __name__ == "__main__":
    sys.exit(main())
