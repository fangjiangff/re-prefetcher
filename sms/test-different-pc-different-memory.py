#!/usr/bin/env python3

import argparse
import os
import subprocess
import sys
from datetime import datetime
from pathlib import Path

os.environ.setdefault("MPLCONFIGDIR", "/tmp/matplotlib-cache")
import matplotlib.pyplot as plt
from matplotlib.patches import Patch


HERE = Path(__file__).resolve().parent
SRC = HERE / "test-different-pc-different-memory.c"
BIN_DIR = HERE / "bin"
BIN = BIN_DIR / "test-different-pc-different-memory"
RES_DIR = HERE / "res"

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


def run(cmd):
    print("+", " ".join(str(x) for x in cmd))
    subprocess.run(cmd, check=True)


def parse_result(output):
    sections = {}
    current = None

    for line in output.splitlines():
        line = line.strip()
        if not line:
            continue
        if line.startswith("[") and line.endswith("]"):
            current = line[1:-1]
            sections[current] = []
            continue
        if line.startswith("#") or current is None:
            continue

        parts = line.split()
        if len(parts) != 4 or not parts[0].isdigit():
            continue

        sections[current].append({
            "line": int(parts[0]),
            "role": parts[1],
            "hits": int(parts[2]),
            "per_1000": int(parts[3]),
        })

    return sections


def pretty_section_name(name):
    names = {
        "without_extra_region_accesses": "No extra region accesses",
        "with_16_extra_region_accesses": "With 16 extra region accesses",
        "different_pc_different_memory": "Different PC, different memory",
    }
    return names.get(name, name.replace("_", " "))


def pretty_mode_name(mode):
    names = {
        "load": "load",
        "sw": "software prefetch",
    }
    return names.get(mode, mode)


def legend_handles_for_palette(palette):
    return [
        Patch(facecolor=palette["trigger"], edgecolor="black", label="Trigger line"),
        Patch(facecolor=palette["expected_prefetch"], edgecolor="black", label="Expected prefetch line"),
        Patch(facecolor=palette["."], edgecolor="black", label="Other line"),
    ]


def plot_rows_on_axes(axes, sections):
    palette = {
        ".": "#B8BCC2",
        "expected_prefetch": "#0072B2",
        "trigger": "#D55E00",
    }

    for ax, name in zip(axes, sections.keys()):
        rows = sections[name]
        lines = [row["line"] for row in rows]
        values = [row["per_1000"] for row in rows]
        colors = [palette.get(row["role"], palette["."]) for row in rows]

        ax.bar(lines, values, color=colors, edgecolor="black", linewidth=0.25, width=0.82)
        ax.set_title(pretty_section_name(name), loc="left", pad=4)
        ax.set_ylabel("Hit rate\n(per 1000 probes)")
        ax.set_ylim(0, 1050)
        ax.set_xlim(-0.75, 63.75)
        ax.set_yticks([0, 250, 500, 750, 1000])
        ax.grid(axis="y", color="#D9D9D9", linewidth=0.7)
        ax.set_axisbelow(True)
        ax.spines["top"].set_visible(False)
        ax.spines["right"].set_visible(False)
        ax.tick_params(axis="both", length=3, width=0.8)

    return palette


def plot_result(output, path, title):
    sections = parse_result(output)
    if not sections:
        print("No plottable data found.", file=sys.stderr)
        return None

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

    palette = plot_rows_on_axes(axes, sections)
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


def plot_combined(results_by_mode, path, title):
    parsed = {mode: parse_result(output) for mode, output in results_by_mode.items()}
    parsed = {mode: sections for mode, sections in parsed.items() if sections}
    if not parsed:
        print("No plottable data found.", file=sys.stderr)
        return None

    modes = list(parsed.keys())
    section_names = ["with_16_extra_region_accesses"]
    nrows = len(modes)
    ncols = len(section_names)
    fig, axes = plt.subplots(
        nrows,
        ncols,
        figsize=(7.2 * ncols, 2.55 * nrows),
        sharex=True,
        sharey=True,
    )
    if nrows == 1 and ncols == 1:
        axes = [[axes]]
    elif nrows == 1:
        axes = [axes]
    elif ncols == 1:
        axes = [[ax] for ax in axes]

    palette = {
        ".": "#B8BCC2",
        "expected_prefetch": "#0072B2",
        "trigger": "#D55E00",
    }

    for row_idx, mode in enumerate(modes):
        for col_idx, section in enumerate(section_names):
            ax = axes[row_idx][col_idx]
            rows = parsed[mode].get(section, [])
            lines = [row["line"] for row in rows]
            values = [row["per_1000"] for row in rows]
            colors = [palette.get(row["role"], palette["."]) for row in rows]

            ax.bar(lines, values, color=colors, edgecolor="black", linewidth=0.25, width=0.82)
            mode_label = pretty_mode_name(mode)
            subtitle = mode_label
            if ncols > 1:
                subtitle = f"{mode_label} - {pretty_section_name(section)}"
            ax.set_title(subtitle, loc="left", pad=4)
            if col_idx == 0:
                ax.set_ylabel("Hit rate\n(per 1000 probes)")
            ax.set_ylim(0, 1050)
            ax.set_xlim(-0.75, 63.75)
            ax.set_yticks([0, 250, 500, 750, 1000])
            ax.grid(axis="y", color="#D9D9D9", linewidth=0.7)
            ax.set_axisbelow(True)
            ax.spines["top"].set_visible(False)
            ax.spines["right"].set_visible(False)
            ax.tick_params(axis="both", length=3, width=0.8)

    for ax in axes[-1]:
        ax.set_xlabel("Cache line index in test region")
        ax.set_xticks(list(range(0, 64, 4)))

    fig.suptitle(title, fontsize=12, fontweight="bold", y=0.965)
    axes[0][0].legend(
        handles=legend_handles_for_palette(palette),
        ncol=3,
        loc="upper right",
        frameon=False,
        handlelength=1.3,
        columnspacing=1.2,
    )
    fig.tight_layout(rect=(0, 0, 1, 0.955))
    fig.savefig(path)
    plt.close(fig)
    return True


def output_path_for_mode(path_arg, mode, timestamp, suffix):
    if path_arg is None:
        return RES_DIR / f"different_pc_different_memory-{mode}-{timestamp}.{suffix}"

    path = Path(path_arg)
    if suffix == "txt":
        expected_suffix = ".txt"
    else:
        expected_suffix = ".png"

    if path.suffix != expected_suffix:
        path = path.with_suffix(expected_suffix)

    return path.with_name(f"{path.stem}-{mode}{path.suffix}")


def combined_plot_path(path_arg, timestamp):
    if path_arg is None:
        return RES_DIR / f"different_pc_different_memory-combined-{timestamp}.png"

    path = Path(path_arg)
    if path.suffix != ".png":
        path = path.with_suffix(".png")
    return path.with_name(f"{path.stem}-combined{path.suffix}")


def run_one_mode(args, mode, timestamp, save_individual_plot):
    cmd = [
        "taskset",
        "-c",
        str(args.core),
        BIN,
        str(args.rounds),
        str(args.threshold_ns),
        mode,
    ]

    env = os.environ.copy()
    env.setdefault("LC_ALL", "C")
    print("+", " ".join(str(x) for x in cmd))
    result = subprocess.run(cmd, check=True, env=env, capture_output=True, text=True)

    if result.stdout:
        print(result.stdout, end="")
    if result.stderr:
        print(result.stderr, end="", file=sys.stderr)

    result_path = output_path_for_mode(args.output, mode, timestamp, "txt")
    plot_path = output_path_for_mode(args.plot, mode, timestamp, "png")

    result_path.parent.mkdir(parents=True, exist_ok=True)
    result_path.write_text(result.stdout)
    print(f"Saved result to {result_path}")

    plot_path.parent.mkdir(parents=True, exist_ok=True)
    if save_individual_plot:
        plotted = plot_result(result.stdout, plot_path, f"test-different_pc_different_memory ({pretty_mode_name(mode)})")
        print(f"Saved plot to {plot_path}")
        if not plotted:
            print("Plot was not generated.", file=sys.stderr)

    return result.stdout


def main():
    parser = argparse.ArgumentParser(
        description="Build and run the SMS different-PC different-memory test."
    )
    parser.add_argument("--rounds", type=int, default=40000)
    parser.add_argument("--threshold-ns", type=int, default=150)
    parser.add_argument("--core", type=int, default=0)
    parser.add_argument("--access", choices=["both", "load", "sw"], default="both")
    parser.add_argument("--output", default=None)
    parser.add_argument("--plot", default=None)
    parser.add_argument("--individual-plots", action="store_true")
    parser.add_argument("--no-build", action="store_true")
    parser.add_argument("--build-only", action="store_true")
    args = parser.parse_args()

    if not args.no_build:
        BIN_DIR.mkdir(exist_ok=True)
        run([
            "g++",
            "-x",
            "c",
            "-std=gnu11",
            "-O0",
            "-Wall",
            "-Wextra",
            "-static",
            "-o",
            BIN,
            SRC,
        ])

    if args.build_only:
        return 0

    if not BIN.exists():
        raise SystemExit(f"binary not found: {BIN}")

    RES_DIR.mkdir(exist_ok=True)
    timestamp = datetime.now().strftime("%Y%m%d-%H%M%S")

    modes = ["load", "sw"] if args.access == "both" else [args.access]
    outputs = {}
    for mode in modes:
        outputs[mode] = run_one_mode(args, mode, timestamp, args.individual_plots or len(modes) == 1)

    if len(outputs) > 1:
        plot_path = combined_plot_path(args.plot, timestamp)
        plot_path.parent.mkdir(parents=True, exist_ok=True)
        plotted = plot_combined(outputs, plot_path, "test-different_pc_different_memory (load vs software prefetch)")
        print(f"Saved combined plot to {plot_path}")
        if not plotted:
            print("Combined plot was not generated.", file=sys.stderr)

    return 0


if __name__ == "__main__":
    sys.exit(main())
