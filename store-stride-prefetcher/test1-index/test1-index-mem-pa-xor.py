import argparse
import csv
import os
import re
import subprocess
import sys
import math

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
)


BASE_DIR = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.join(BASE_DIR, "test1-index-mem-pa-xor.c")
UTIL_SRC = os.path.join(ROOT_DIR, "until.c")
OUT = os.path.join(ROOT_DIR, "bin", "test1-index-mem-pa-xor")

DEFAULT_BUDDY_MB = 64
DEFAULT_STRIDE_LINES = 5
DEFAULT_MIN_DIFF_BIT = 12
DEFAULT_MAX_DIFF_BIT = 47
DEFAULT_ROUNDS = 400
DEFAULT_PROBE_POSITIONS = 64
RESULT_DIR = os.path.join(ROOT_DIR, "res", "index-mem-pa-xor")
PLOT_DIR = os.path.join(ROOT_DIR, "res", "barplots")
PMU_COUNTER_NAME = "l2d_cache_hwprf"
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
         "-march=armv8.5-a+predres",
        f"-DARCH_NAME=\"{args.arch}\"",
        f"-DBUDDY_PAGES={(args.buddy_mb * 1024 * 1024) // 4096}",
        f"-DSTRIDE_LINES={args.stride}",
        f"-DSTORE_ACCESSES={args.accesses}",
        f"-DMIN_DIFF_BIT={args.min_diff_bit}",
        f"-DMAX_DIFF_BIT={args.max_diff_bit}",
        f"-DROUNDS={args.rounds}",
        f"-DPROBE_POSITIONS={args.probe_positions}",
        f"-DENABLE_CPP_RCTX={1 if args.arch in ('X925', 'A725') else 0}",
        f"-DENABLE_PMU={args.enable_pmu}",
        f"-DENABLE_PROBE={args.enable_probe}",
        f"-DPMU_CORE_X925={1 if args.arch == 'X925' else 0}",
        f"-DPMU_CORE_A55={1 if args.arch == 'A55' else 0}",
        "-o",
        OUT,
        SRC,
        UTIL_SRC,
    ]
    return subprocess.run(compile_cmd)


def run_test(args):
    run_cmd = ["taskset", "-c", str(args.core), OUT]
    env = os.environ.copy()
    if args.pmu_device:
        env["PMU_DEVICE"] = args.pmu_device
    return subprocess.run(run_cmd, capture_output=True, text=True, env=env)


def micro_arch_name(args):
    return (
        f"{args.arch}-core{args.core}-index-mem-pa-xor"
        f"-stride{args.stride}-accesses{args.accesses}"
        f"-probe{args.probe_positions}"
        f"-pmu{args.enable_pmu}-probe-enabled{args.enable_probe}"
        f"-bits{args.min_diff_bit}-{args.max_diff_bit}"
        f"-buddy{args.buddy_mb}MB"
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
        PLOT_DIR, f"{micro_arch_name(args)}-{PMU_COUNTER_NAME}.png"
    )


def ensure_parent(path):
    parent = os.path.dirname(path)
    if parent:
        os.makedirs(parent, exist_ok=True)


def require_sudo():
    if hasattr(os, "geteuid") and os.geteuid() != 0:
        cmd = " ".join(["sudo", sys.executable] + sys.argv)
        print("This test reads physical addresses from /proc/self/pagemap.", file=sys.stderr)
        print("Please run with sudo: " + cmd, file=sys.stderr)
        return 1
    return 0


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


def read_tsv(path):
    with open(path, newline="") as f:
        reader = csv.DictReader(f, delimiter="\t")
        rows = []
        for flat in reader:
            row = {
                "case": flat["case"],
                "latency_ns": int(flat["latency_ns"]),
                "prefetched": flat["prefetched"],
                "threshold_ns": int(flat["threshold_ns"]),
                "pmu": {},
            }
            for name, value in flat.items():
                if name not in {"case", "latency_ns", "prefetched", "threshold_ns"} and value:
                    row["pmu"][name] = float(value)
            rows.append(row)
    if not rows:
        raise ValueError("no TSV rows found")
    return rows


def split_plot_paths(path):
    root, ext = os.path.splitext(path)
    if not ext:
        ext = ".png"
    return {
        "single": f"{root}-single{ext}",
        "pairwise": f"{root}-pairwise-heatmap{ext}",
        "targeted": f"{root}-targeted{ext}",
    }


def parse_single_bit_case(name):
    parts = name.split("_")
    if len(parts) == 3 and parts[0] == "bit" and parts[2] == "full":
        return int(parts[1])
    return None


def parse_pairwise_case(name):
    parts = name.split("_")
    if len(parts) == 4 and parts[0] == "bits" and parts[3] == "full":
        return int(parts[1]), int(parts[2])
    return None


def is_targeted_case(name):
    return name.startswith("triple_") or name.startswith("pairpair_")


def plot_bar(path, rows, title, threshold_ns, xlabel, vmin=None, vmax=None):
    ensure_parent(path)
    labels = [row["case"] for row in rows]
    values = [max(0, int(row["latency_ns"])) for row in rows]
    colors = ["#0072B2" if row["prefetched"] == "yes" else "#BDBDBD" for row in rows]
    if vmin is None:
        vmin = 0
    if vmax is None:
        vmax = max(max(values) if values else 0, threshold_ns) * 1.12

    width = max(10.5, min(42.0, 0.24 * len(rows) + 3.0))
    fig, ax = plt.subplots(figsize=(width, 5.2))
    ax.bar(range(len(rows)), values, color=colors, edgecolor="black", linewidth=0.25)
    ax.axhline(threshold_ns, color="black", linestyle="--", linewidth=1.0)
    ax.set_title(title, loc="left", pad=8)
    ax.set_xlabel(xlabel)
    ax.set_ylabel("latency (ns)")
    ax.set_xticks(range(len(rows)))
    ax.set_xticklabels(labels, rotation=65, ha="right", fontsize=7)
    ax.set_ylim(vmin, vmax)
    ax.grid(axis="y", alpha=0.25)
    fig.tight_layout()
    fig.savefig(path, dpi=300)
    plt.close(fig)


def plot_pairwise_heatmap(path, rows, title, threshold_ns, bit_min=13, bit_max=33):
    ensure_parent(path)
    bits = list(range(bit_min, bit_max + 1))
    index = {bit: pos for pos, bit in enumerate(bits)}
    matrix = [[math.nan for _ in bits] for _ in bits]
    highlighted_pairs = {(16, 22), (16, 28)}
    highlight_cells = []

    for row in rows:
        parsed = parse_pairwise_case(row["case"])
        if not parsed:
            continue
        bit_a, bit_b = parsed
        if bit_a not in index or bit_b not in index:
            continue
        value = int(row["latency_ns"])
        if value < 0:
            continue
        upper_bit = min(bit_a, bit_b)
        lower_bit = max(bit_a, bit_b)
        upper = index[upper_bit]
        lower = index[lower_bit]
        matrix[lower][upper] = value
        if (upper_bit, lower_bit) in highlighted_pairs:
            highlight_cells.append((lower, upper))

    values = [v for row in matrix for v in row if not math.isnan(v)]
    # vmax = max(max(values) if values else 0, threshold_ns) * 1.05
    vmax = 200
    fig, ax = plt.subplots(figsize=(9.5, 8.5))
    cmap = plt.get_cmap("viridis_r").copy()
    cmap.set_bad(color="#E6E8EB")
    image = ax.imshow(matrix, cmap=cmap, vmin=0, vmax=vmax)
    ax.set_title(title + " (lower triangle)", loc="left", pad=10, fontsize=16)
    ax.set_xlabel("PA bit", fontsize=14)
    ax.set_ylabel("PA bit", fontsize=14)
    ax.set_xticks(range(len(bits)))
    ax.set_yticks(range(len(bits)))
    ax.set_xticklabels(bits, rotation=90, fontsize=11)
    ax.set_yticklabels(bits, fontsize=11)
    ax.tick_params(axis="both", which="major", length=0)
    bottom_axis = len(bits) - 0.5
    # for lower, upper in highlight_cells:
    #     ax.plot([upper, upper], [lower, bottom_axis], color="#D62728",
    #             linestyle="--", linewidth=1.2, alpha=0.95, zorder=3)
    #     ax.plot([-0.5, upper], [lower, lower], color="#D62728",
    #             linestyle="--", linewidth=1.2, alpha=0.95, zorder=3)
    for spine in ax.spines.values():
        spine.set_visible(False)

    cbar = fig.colorbar(image, ax=ax, fraction=0.046, pad=0.04)
    cbar.set_label("latency (ns)", fontsize=13)
    cbar.ax.tick_params(labelsize=11)
    fig.tight_layout()
    fig.savefig(path, dpi=300)
    plt.close(fig)


def plot_result(path, rows, title, threshold_ns, vmin=None, vmax=None):
    if not path:
        return []

    paths = split_plot_paths(path)
    baseline_rows = [row for row in rows if row["case"].startswith("same_pa_")]
    single_rows = [row for row in rows if parse_single_bit_case(row["case"]) is not None]
    single_rows.sort(key=lambda row: parse_single_bit_case(row["case"]))
    pairwise_rows = [row for row in rows if parse_pairwise_case(row["case"]) is not None]
    targeted_rows = [row for row in rows if is_targeted_case(row["case"])]

    plot_bar(
        paths["single"],
        baseline_rows + single_rows,
        f"{title}: single-bit controls",
        threshold_ns,
        "baseline / flipped PA bit",
        vmin=vmin,
        vmax=vmax,
    )
    plot_pairwise_heatmap(
        paths["pairwise"],
        pairwise_rows,
        f"{title}: pairwise PA bit flips (13..33)",
        threshold_ns,
    )
    plot_bar(
        paths["targeted"],
        targeted_rows,
        f"{title}: triple and pairpair targeted masks",
        threshold_ns,
        "targeted mask case",
        vmin=vmin,
        vmax=vmax,
    )
    return [paths["single"], paths["pairwise"], paths["targeted"]]


def plot_pmu_line(path, rows, title, xlabel):
    ensure_parent(path)
    labels = [row["case"] for row in rows]
    values = [
        row["pmu"].get(PMU_COUNTER_NAME, math.nan)
        for row in rows
    ]
    valid_values = [value for value in values if not math.isnan(value)]
    vmax = max(valid_values) * 1.12 if valid_values else 1.0
    if vmax <= 0:
        vmax = 1.0

    width = max(10.5, min(42.0, 0.24 * len(rows) + 3.0))
    fig, ax = plt.subplots(figsize=(width, 5.2))
    ax.plot(
        range(len(rows)),
        values,
        color="#0072B2",
        marker="o",
        linewidth=1.5,
        markersize=3.5,
    )
    ax.set_title(title, loc="left", pad=8)
    ax.set_xlabel(xlabel)
    ax.set_ylabel(f"{PMU_COUNTER_NAME} / round")
    ax.set_xticks(range(len(rows)))
    ax.set_xticklabels(labels, rotation=65, ha="right", fontsize=7)
    ax.set_ylim(0, vmax)
    ax.grid(axis="y", alpha=0.25)
    fig.tight_layout()
    fig.savefig(path, dpi=300)
    plt.close(fig)


def plot_pmu_pairwise_heatmap(path, rows, title, bit_min=13, bit_max=33):
    ensure_parent(path)
    bits = list(range(bit_min, bit_max + 1))
    index = {bit: pos for pos, bit in enumerate(bits)}
    matrix = [[math.nan for _ in bits] for _ in bits]

    for row in rows:
        parsed = parse_pairwise_case(row["case"])
        value = row["pmu"].get(PMU_COUNTER_NAME)
        if not parsed or value is None:
            continue
        bit_a, bit_b = parsed
        if bit_a not in index or bit_b not in index:
            continue
        upper = index[min(bit_a, bit_b)]
        lower = index[max(bit_a, bit_b)]
        matrix[lower][upper] = value

    values = [value for line in matrix for value in line if not math.isnan(value)]
    vmax = max(values) * 1.05 if values else 1.0
    if vmax <= 0:
        vmax = 1.0
    fig, ax = plt.subplots(figsize=(9.5, 8.5))
    cmap = plt.get_cmap("viridis_r").copy()
    cmap.set_bad(color="#E6E8EB")
    image = ax.imshow(matrix, cmap=cmap, vmin=0, vmax=vmax)
    ax.set_title(title + " (lower triangle)", loc="left", pad=10, fontsize=16)
    ax.set_xlabel("PA bit", fontsize=14)
    ax.set_ylabel("PA bit", fontsize=14)
    ax.set_xticks(range(len(bits)))
    ax.set_yticks(range(len(bits)))
    ax.set_xticklabels(bits, rotation=90, fontsize=11)
    ax.set_yticklabels(bits, fontsize=11)
    ax.tick_params(axis="both", which="major", length=0)
    for spine in ax.spines.values():
        spine.set_visible(False)
    cbar = fig.colorbar(image, ax=ax, fraction=0.046, pad=0.04)
    cbar.set_label(f"{PMU_COUNTER_NAME} / round", fontsize=13)
    cbar.ax.tick_params(labelsize=11)
    fig.tight_layout()
    fig.savefig(path, dpi=300)
    plt.close(fig)


def plot_pmu_result(path, rows, title):
    if not any(PMU_COUNTER_NAME in row["pmu"] for row in rows):
        raise ValueError(f"PMU counter {PMU_COUNTER_NAME} is unavailable")

    paths = split_plot_paths(path)
    baseline_rows = [row for row in rows if row["case"].startswith("same_pa_")]
    single_rows = [
        row for row in rows if parse_single_bit_case(row["case"]) is not None
    ]
    single_rows.sort(key=lambda row: parse_single_bit_case(row["case"]))
    pairwise_rows = [
        row for row in rows if parse_pairwise_case(row["case"]) is not None
    ]
    targeted_rows = [row for row in rows if is_targeted_case(row["case"])]

    plot_pmu_line(
        paths["single"],
        baseline_rows + single_rows,
        f"{title}: single-bit controls",
        "baseline / flipped PA bit",
    )
    plot_pmu_pairwise_heatmap(
        paths["pairwise"],
        pairwise_rows,
        f"{title}: pairwise PA bit flips (13..33)",
    )
    plot_pmu_line(
        paths["targeted"],
        targeted_rows,
        f"{title}: triple and pairpair targeted masks",
        "targeted mask case",
    )
    return [paths["single"], paths["pairwise"], paths["targeted"]]


def main():
    parser = argparse.ArgumentParser(
        description="Test whether PA bit xor masks preserve store-stride prefetcher indexing."
    )
    parser.add_argument("--arch", required=True, choices=arch_choices())
    parser.add_argument("--core", type=int, default=None,
                        help="Override CPU core. Default comes from --arch.")
    parser.add_argument("--buddy-mb", type=int, default=DEFAULT_BUDDY_MB,
                        help=f"Anonymous page pool size in MiB. Default: {DEFAULT_BUDDY_MB}")
    parser.add_argument("--stride", type=int, default=DEFAULT_STRIDE_LINES,
                        help="Stride in cache lines. Default: 5.")
    parser.add_argument("--accesses", type=int, default=None,
                        help="Total store train+trigger accesses. Default is "
                             "selected from --arch store accesses.")
    parser.add_argument("--min-diff-bit", type=int, default=DEFAULT_MIN_DIFF_BIT)
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
    parser.add_argument("--cc", default=os.environ.get("CC", "gcc"))
    parser.add_argument("--output", default=None)
    parser.add_argument("--raw-output", default=None)
    parser.add_argument("--plot-output", default=None)
    parser.add_argument("--pmu-plot-output", default=None)
    parser.add_argument("--input-tsv", default=None,
                        help="Read an existing TSV and only regenerate plots.")
    parser.add_argument("--plot-vmin", type=float, default=None)
    parser.add_argument("--plot-vmax", type=float, default=200)
    parser.add_argument("--no-plot", action="store_true")
    parser.add_argument("--no-compile", action="store_true")
    args = parser.parse_args()
    args.access = "store"
    if args.enable_probe is None:
        args.enable_probe = 0 if args.enable_pmu else 1

    if args.input_tsv:
        apply_single_core_defaults(args)
        apply_access_defaults(args)
        try:
            rows = read_tsv(args.input_tsv)
        except ValueError as exc:
            print(f"Read TSV failed: {exc}", file=sys.stderr)
            return 1
        threshold_ns = args.threshold_ns or int(rows[0].get("threshold_ns", 0))
        if threshold_ns < 1:
            parser.error("--threshold-ns must be provided when TSV has no valid threshold")
        plot_title = f"{args.arch}: PA xor-index contribution"
        if not args.no_plot and args.enable_pmu:
            try:
                paths = plot_pmu_result(
                    pmu_plot_path(args),
                    rows,
                    f"{plot_title}: {PMU_COUNTER_NAME}",
                )
            except ValueError as exc:
                print(f"PMU plot failed: {exc}", file=sys.stderr)
                return 1
            for path in paths:
                print(f"Saved PMU plot to {path}")
        if not args.no_plot and args.enable_probe:
            paths = plot_result(
                plot_path(args),
                rows,
                plot_title,
                threshold_ns,
                vmin=args.plot_vmin,
                vmax=args.plot_vmax,
            )
            for path in paths:
                print(f"Saved plot to {path}")
        return 0

    sudo_error = require_sudo()
    if sudo_error:
        return sudo_error

    apply_single_core_defaults(args)
    apply_access_defaults(args)
    apply_threshold_defaults(args)

    if args.core < 0:
        parser.error("--core must be >= 0")
    if args.buddy_mb < 1:
        parser.error("--buddy-mb must be >= 1")
    if args.stride < 1:
        parser.error("--stride must be >= 1")
    if args.accesses < 2:
        parser.error("--accesses must be >= 2")
    if args.min_diff_bit < 12:
        parser.error("--min-diff-bit must be >= 12")
    if args.max_diff_bit < args.min_diff_bit or args.max_diff_bit >= 48:
        parser.error("--max-diff-bit must be in [min_diff_bit, 47]")
    if args.rounds < 1:
        parser.error("--rounds must be >= 1")
    if args.probe_positions < 1:
        parser.error("--probe-positions must be >= 1")
    predicted_pos = args.accesses * args.stride
    trigger_pos = (args.accesses - 1) * args.stride
    if (trigger_pos + 1) * 64 > 4096:
        parser.error("trigger position must fit inside one 4 KiB page")
    if args.enable_probe and predicted_pos >= args.probe_positions:
        parser.error("predicted position accesses * stride must be inside probe positions")
    if args.enable_probe and (predicted_pos + 1) * 64 > 4096:
        parser.error("predicted position must fit inside one 4 KiB page")
    if args.threshold_ns < 1:
        parser.error("--threshold-ns must be >= 1")

    print(
        f"arch={args.arch}, core={args.core}, buddy_mb={args.buddy_mb}, "
        f"stride={args.stride}, accesses={args.accesses}, "
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
        pmu_title = (
            f"{args.arch} core {args.core}: PA xor-index {PMU_COUNTER_NAME} "
            f"(stride={args.stride}, accesses={args.accesses}, "
            f"buddy={args.buddy_mb}MB)"
        )
        try:
            pmu_paths = plot_pmu_result(pmu_plot_path(args), rows, pmu_title)
        except ValueError as exc:
            print(f"PMU plot failed: {exc}", file=sys.stderr)
            return 1
        for path in pmu_paths:
            print(f"Saved PMU plot to {path}")

    if not args.no_plot and args.enable_probe:
        plot_title = (
            f"{args.arch} core {args.core}: PA xor-index contribution "
            f"(stride={args.stride}, accesses={args.accesses}, "
            f"buddy={args.buddy_mb}MB)"
        )
        paths = plot_result(
            plot_path(args),
            rows,
            plot_title,
            args.threshold_ns,
            vmin=args.plot_vmin,
            vmax=args.plot_vmax,
        )
        for path in paths:
            print(f"Saved plot to {path}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
