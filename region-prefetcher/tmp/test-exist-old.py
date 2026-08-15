#!/usr/bin/env python3
import argparse
import os
import re
import subprocess
import sys
from pathlib import Path

os.environ.setdefault("MPLCONFIGDIR", "/tmp/matplotlib-cache")
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib.patches import Patch

plt.rcParams.update({
    "figure.dpi": 150,
    "savefig.dpi": 300,
    "font.family": "DejaVu Sans",
    "font.size": 10,
    "axes.labelsize": 10,
    "axes.titlesize": 11,
    "legend.fontsize": 9,
    "xtick.labelsize": 9,
    "ytick.labelsize": 9,
    "axes.linewidth": 0.8,
    "pdf.fonttype": 42,
    "ps.fonttype": 42,
})

HERE = Path(__file__).resolve().parent
REPO = HERE.parent
SRC = HERE / "test-exist-old.c"
UTIL_SRC = REPO / "store-stride-prefetcher" / "until.c"
UTIL_INC = REPO / "store-stride-prefetcher"
BIN_DIR = REPO / "bin"
BIN = BIN_DIR / "region-test-exist-old"
RESULT_DIR = REPO / "res" / "region-prefetcher"
DEFAULT_FOOTPRINT_LINES = [4, 19, 6, 27, 13, 1, 15]
DEFAULT_TRIGGER_LINES = [3]

PMU_RE = re.compile(r"^# PMU\s+(\S+)\s+per_round=([0-9.]+)")
PROBE_RE = re.compile(
    r"^# PROBE\s+(\d+)\s+(\S+)\s+(\d+)\s+(\d+)\s+(\d+)"
)


def format_line_list(lines):
    return ",".join(str(line) for line in lines)


def parse_line_list(value):
    return [int(item.strip()) for item in value.split(",") if item.strip()]


def parse_args():
    parser = argparse.ArgumentParser(
        description="Build and run region-prefetcher/test-exist.c."
    )
    parser.add_argument("--cc", default="gcc", help="C compiler. Default: gcc")
    parser.add_argument("--cpu", type=int, default=6,
                        help="CPU used by taskset. Default: 6")
    parser.add_argument("--rounds", type=int, default=4000,
                        help="ROUNDS macro. Default: 4000")
    parser.add_argument("--train-regions", type=int, default=32,
                        help="TRAIN_REGIONS macro. Default: 32")
    parser.add_argument("--wait-nops", type=int, default=1000,
                        help="PREFETCH_WAIT_NOPS macro. Default: 1000")
    parser.add_argument("--wait-ns", type=int, default=10000,
                        help="PREFETCH_WAIT_NS macro. Default: 10000")
    parser.add_argument("--pmu-settle-nops", type=int, default=0,
                        help="Extra nops before PMU stop. Default: 0")
    parser.add_argument("--pmu-settle-ns", type=int, default=0,
                        help="Extra nanosleep before PMU stop. Default: 0")
    parser.add_argument("--cache-probe", action=argparse.BooleanOptionalAction,
                        default=True,
                        help="Enable cache probe output. Default: enabled")
    parser.add_argument("--hit-threshold", type=int, default=200,
                        help="CACHE_HIT_THRESHOLD_TICKS macro. Default: 200")
    parser.add_argument("--pmu-device", default=None,
                        help="Optional PMU_DEVICE environment override.")
    parser.add_argument("--footprint-lines", default=format_line_list(DEFAULT_FOOTPRINT_LINES),
                        help=("Comma-separated cache lines used only for plot annotations. "
                              f"Default: {format_line_list(DEFAULT_FOOTPRINT_LINES)}"))
    parser.add_argument("--trigger-lines", default=format_line_list(DEFAULT_TRIGGER_LINES),
                        help=("Comma-separated trigger cache lines used only for plot annotations. "
                              f"Default: {format_line_list(DEFAULT_TRIGGER_LINES)}"))
    parser.add_argument("--prefetch-latency-threshold", type=int, default=100,
                        help="Mark non-trigger lines with avg_latency below this value as prefetched. Default: 100")
    parser.add_argument("--output", default=None,
                        help="Write program output to this file instead of stdout.")
    parser.add_argument("--plot", action=argparse.BooleanOptionalAction,
                        default=True,
                        help="Generate bar plots. Default: enabled")
    parser.add_argument("--plot-prefix", default=None,
                        help="Output path prefix for plots. Default: res/region-prefetcher/test-exist")
    parser.add_argument("--no-run", action="store_true",
                        help="Only build the binary.")
    args = parser.parse_args()

    for name in ("rounds", "train_regions", "wait_nops", "wait_ns", "pmu_settle_nops",
                 "pmu_settle_ns", "hit_threshold",
                 "prefetch_latency_threshold"):
        if getattr(args, name) < 0:
            parser.error(f"--{name.replace('_', '-')} must be >= 0")
    if args.rounds < 1:
        parser.error("--rounds must be >= 1")
    if args.train_regions < 1:
        parser.error("--train-regions must be >= 1")
    if args.cpu < 0:
        parser.error("--cpu must be >= 0")
    try:
        args.footprint_lines_parsed = parse_line_list(args.footprint_lines)
    except ValueError:
        parser.error("--footprint-lines must contain comma-separated integers")
    try:
        args.trigger_lines_parsed = parse_line_list(args.trigger_lines)
    except ValueError:
        parser.error("--trigger-lines must contain comma-separated integers")
    if not args.footprint_lines_parsed:
        parser.error("--footprint-lines must not be empty")
    if not args.trigger_lines_parsed:
        parser.error("--trigger-lines must not be empty")
    bad_lines = [
        line for line in args.footprint_lines_parsed + args.trigger_lines_parsed
        if line < 0 or line >= 64
    ]
    if bad_lines:
        parser.error("--footprint-lines and --trigger-lines entries must be in [0, 63]")
    return args


def c_initializer(lines):
    return "{" + ",".join(str(line) for line in lines) + "}"


def build(args, bin_path, enable_pmu, enable_cache_probe):
    BIN_DIR.mkdir(parents=True, exist_ok=True)
    cmd = [
        args.cc,
        "-O2",
        "-Wall",
        "-Wextra",
        "-march=armv8.5-a+predres",
        f"-DROUNDS={args.rounds}",
        f"-DTRAIN_REGIONS={args.train_regions}",
        f"-DPREFETCH_WAIT_NOPS={args.wait_nops}",
        f"-DPREFETCH_WAIT_NS={args.wait_ns}",
        f"-DPMU_SETTLE_WAIT_NOPS={args.pmu_settle_nops}",
        f"-DPMU_SETTLE_WAIT_NS={args.pmu_settle_ns}",
        f"-DENABLE_PMU={1 if enable_pmu else 0}",
        f"-DENABLE_CACHE_PROBE={1 if enable_cache_probe else 0}",
        f"-DCACHE_HIT_THRESHOLD_TICKS={args.hit_threshold}",
        f"-DFOOTPRINT_LINES={c_initializer(args.footprint_lines_parsed)}",
        f"-DTRIGGER_LINES={c_initializer(args.trigger_lines_parsed)}",
        f"-I{UTIL_INC}",
        str(SRC),
        str(UTIL_SRC),
        "-o",
        str(bin_path),
    ]
    if enable_pmu and enable_cache_probe:
        mode = "combined"
    elif enable_pmu:
        mode = "pmu-only"
    else:
        mode = "cache-only"
    print(f"# build {mode}:", " ".join(cmd))
    subprocess.run(cmd, check=True)


def run_binary(args, bin_path, mode):
    cmd = ["taskset", "-c", str(args.cpu), str(bin_path)]
    env = os.environ.copy()
    if args.pmu_device:
        env["PMU_DEVICE"] = args.pmu_device

    print(f"# run {mode}:", " ".join(cmd))
    result = subprocess.run(cmd, check=False, text=True,
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                            env=env)
    return result.returncode, result.stdout


def output_paths(args):
    if args.output:
        combined = Path(args.output)
    else:
        RESULT_DIR.mkdir(parents=True, exist_ok=True)
        combined = RESULT_DIR / "test-exist-old.txt"
    combined.parent.mkdir(parents=True, exist_ok=True)
    pmu = combined.with_name(combined.stem + "-pmu" + combined.suffix)
    cache = combined.with_name(combined.stem + "-cache" + combined.suffix)
    return combined, pmu, cache


def save_output(args, text):
    combined_path, _, _ = output_paths(args)
    combined_path.write_text(text)
    print(f"# output: {combined_path}")


def parse_output(text):
    current = None
    pmu = {}
    probes = {}

    for line in text.splitlines():
        if line.startswith("[") and line.endswith("]"):
            current = line.strip("[]")
            pmu.setdefault(current, {})
            probes.setdefault(current, {})
            continue
        if current is None:
            continue

        m = PMU_RE.match(line)
        if m:
            pmu[current][m.group(1)] = float(m.group(2))
            continue

        m = PROBE_RE.match(line)
        if m:
            role = m.group(2)
            if role == "direct":
                role = "trigger"
            elif role == "footprint_candidate":
                role = "expected_prefetch"
            else:
                role = "."
            probes[current][int(m.group(1))] = {
                "role": role,
                "avg_latency": int(m.group(3)),
                "samples": int(m.group(4)),
                "hits_per_1000": int(m.group(5)),
            }
    return pmu, probes


def plot_pmu(pmu, path):
    events = sorted({event for case in pmu.values() for event in case})
    cases = list(pmu)
    if not events or not cases:
        return False

    width = 0.8 / max(1, len(cases))
    xs = list(range(len(events)))
    fig, ax = plt.subplots(figsize=(8, 4.5))
    for idx, case in enumerate(cases):
        vals = [pmu[case].get(event, 0.0) for event in events]
        offset = (idx - (len(cases) - 1) / 2) * width
        ax.bar([x + offset for x in xs], vals, width, label=case)
    ax.set_xticks(xs)
    ax.set_xticklabels(events, rotation=20, ha="right")
    ax.set_ylabel("PMU events / round")
    ax.set_title("Region prefetch PMU")
    ax.legend(fontsize=8)
    ax.grid(axis="y", alpha=0.25)
    fig.tight_layout()
    fig.savefig(path, dpi=160)
    plt.close(fig)
    return True


def pretty_section_name(name):
    names = {
        "untrained_pc_control_trigger_4_19": "Untrained PC control",
        "pattern_trigger_4_19": "Trained pattern trigger",
    }
    if name.startswith("untrained_pc_control_trigger_"):
        return "Untrained PC control"
    if name.startswith("pattern_trigger_"):
        return "Trained pattern trigger"
    return names.get(name, name.replace("_", " "))


def legend_handles_for_palette(palette):
    return [
        Patch(facecolor=palette["trigger"], edgecolor="black", label="Trigger line"),
        Patch(facecolor=palette["expected_prefetch"], edgecolor="black", label="Footprint line"),
        Patch(facecolor=palette["prefetched"], edgecolor="black", label="Prefetched line"),
        Patch(facecolor=palette["."], edgecolor="black", label="Other line"),
    ]


def format_delta(line, base_line):
    delta = line - base_line
    if delta > 0:
        return f"+{delta}"
    return str(delta)


def format_trigger_deltas(line, trigger_lines):
    return "/".join(format_delta(line, trigger_line) for trigger_line in trigger_lines)


def annotated_role(section_name, line, row, footprint_set, trigger_set, threshold):
    if line in trigger_set:
        return "trigger"
    if section_name.startswith("pattern_trigger_") and row["avg_latency"] < threshold:
        return "prefetched"
    if line in footprint_set:
        return "expected_prefetch"
    return "."


def plot_probe_rows(probes, pmu, path, title, footprint_lines, trigger_lines,
                    prefetch_latency_threshold):
    sections = {name: rows for name, rows in probes.items() if rows}
    if not sections:
        print("No plottable cache-probe data found.", file=sys.stderr)
        return False

    names = list(sections.keys())
    fig, axes = plt.subplots(
        len(names),
        1,
        figsize=(7.2, 2.35 * len(names)),
        sharex=True,
        constrained_layout=True,
    )
    if len(names) == 1:
        axes = [axes]

    first_access_line = footprint_lines[0]
    footprint_set = set(footprint_lines)
    trigger_set = set(trigger_lines)
    palette = {
        ".": "#B8BCC2",
        "expected_prefetch": "#0072B2",
        "prefetched": "#009E73",
        "trigger": "#D55E00",
    }

    for ax, name in zip(axes, names):
        rows = sections[name]
        lines = sorted(rows)
        values = [rows[line]["avg_latency"] for line in lines]
        roles = [
            annotated_role(name, line, rows[line], footprint_set, trigger_set,
                           prefetch_latency_threshold)
            for line in lines
        ]
        colors = [palette.get(role, palette["."]) for role in roles]

        ymax = max(values) if values else 1
        ax.bar(lines, values, color=colors, edgecolor="black", linewidth=0.25, width=0.82)
        for line, value, role in zip(lines, values, roles):
            should_label = (
                name.startswith("untrained_pc_control_trigger_") and line in footprint_set
            ) or (
                name.startswith("pattern_trigger_") and role == "prefetched"
            )
            if should_label:
                if name.startswith("pattern_trigger_") and role == "prefetched":
                    label = format_trigger_deltas(line, trigger_lines)
                else:
                    label = format_delta(line, first_access_line)
                ax.text(line, value + max(3, ymax * 0.018),
                        label, ha="center", va="bottom", rotation=90, fontsize=8)
        section_title = pretty_section_name(name)
        pmu_items = [
            (event, pmu.get(name, {}).get(event))
            for event in ("l1d_cache_hwprf", "l2d_cache_hwprf")
        ]
        pmu_items = [
            f"{event}={value:.2f}/round"
            for event, value in pmu_items
            if value is not None
        ]
        if name.startswith("pattern_trigger_"):
            control_name = name.replace("pattern_trigger_",
                                        "untrained_pc_control_trigger_", 1)
            pattern_l2 = pmu.get(name, {}).get("l2d_cache_hwprf")
            control_l2 = pmu.get(control_name, {}).get("l2d_cache_hwprf")
            if pattern_l2 is not None and control_l2 is not None:
                pmu_items.append(f"delta_l2d_cache_hwprf={pattern_l2 - control_l2:.2f}/round")
        if pmu_items:
            section_title = f"{section_title}   " + "   ".join(pmu_items)
        ax.set_title(section_title, loc="left", pad=4)
        ax.set_ylabel("Average\nlatency")
        ax.set_ylim(0, max(10, ymax * 1.12))
        ax.set_xlim(-0.75, 63.75)
        ax.grid(axis="y", color="#D9D9D9", linewidth=0.7)
        ax.set_axisbelow(True)
        ax.spines["top"].set_visible(False)
        ax.spines["right"].set_visible(False)
        ax.tick_params(axis="both", length=3, width=0.8)

    fig.suptitle(title, fontsize=12, fontweight="bold")
    axes[0].legend(
        handles=legend_handles_for_palette(palette),
        ncol=3,
        loc="upper right",
        frameon=False,
        handlelength=1.3,
        columnspacing=1.2,
    )
    axes[-1].set_xlabel("Cache line index in test region")
    axes[-1].set_xticks(list(range(0, 64, 4)))

    fig.savefig(path)
    plt.close(fig)
    return True


def make_plots(args, combined_text):
    if not args.plot:
        return
    RESULT_DIR.mkdir(parents=True, exist_ok=True)
    prefix = Path(args.plot_prefix) if args.plot_prefix else RESULT_DIR / "test-exist-old"
    prefix.parent.mkdir(parents=True, exist_ok=True)

    pmu, probes = parse_output(combined_text)
    probe_path = prefix.with_name(prefix.name + "-cache-probe.png")
    trigger_title = format_line_list(args.trigger_lines_parsed)
    footprint_title = format_line_list(args.footprint_lines_parsed)
    if plot_probe_rows(probes, pmu, probe_path,
                       f"[footprint {footprint_title}; trigger {trigger_title}]",
                       args.footprint_lines_parsed, args.trigger_lines_parsed,
                       args.prefetch_latency_threshold):
        print(f"# plot: {probe_path}")


def main():
    args = parse_args()
    combined_bin = BIN_DIR / "region-test-exist-old-combined"

    build(args, combined_bin, enable_pmu=True, enable_cache_probe=True)
    if args.no_run:
        print(f"# binary combined: {combined_bin}")
        return 0

    code, text = run_binary(args, combined_bin, "combined")
    save_output(args, text)
    make_plots(args, text)
    return code


if __name__ == "__main__":
    raise SystemExit(main())
