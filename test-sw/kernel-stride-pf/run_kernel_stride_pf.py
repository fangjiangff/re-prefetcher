#!/usr/bin/env python3

import argparse
import os
import re
import statistics
import subprocess
import sys
from pathlib import Path


SCRIPT_DIR = Path(__file__).resolve().parent
MODULE_NAME = "kernel_stride_pf_probe"
MODULE_FILE = SCRIPT_DIR / f"{MODULE_NAME}.ko"
USER_BIN = SCRIPT_DIR / "kernel_stride_pf_user"


def run_cmd(cmd, *, check=True, capture=False, env=None):
    print("+ " + " ".join(str(part) for part in cmd), flush=True)
    return subprocess.run(
        [str(part) for part in cmd],
        cwd=SCRIPT_DIR,
        check=check,
        capture_output=capture,
        text=True,
        env=env,
    )


def build_targets():
    run_cmd(["make"])


def clean_targets():
    run_cmd(["make", "clean"], check=False)


def unload_module(sudo_cmd, *, quiet=False):
    cmd = sudo_cmd + ["rmmod", MODULE_NAME]
    result = run_cmd(cmd, check=False, capture=quiet)
    if result.returncode != 0 and not quiet:
        print(f"# rmmod returned {result.returncode}; module may not be loaded", file=sys.stderr)


def load_module(sudo_cmd):
    run_cmd(sudo_cmd + ["insmod", MODULE_FILE])


def run_test(core, rounds):
    cmd = ["taskset", "-c", str(core), USER_BIN, str(rounds)]
    result = run_cmd(cmd, capture=True)
    return result.stdout


def parse_tables(text):
    tables = {}
    current = None
    row_re = re.compile(r"^\s*(\d+)\s+(\d+)\s+(\d+)\s+(\S+)\s*$")

    for line in text.splitlines():
        if line.startswith("# "):
            title = line[2:].strip()
            if title.startswith("test:") or title.startswith("control:"):
                current = title
                tables[current] = []
            continue

        if current is None:
            continue

        match = row_re.match(line)
        if not match:
            continue

        tables[current].append(
            {
                "line": int(match.group(1)),
                "offset": int(match.group(2)),
                "cycles": int(match.group(3)),
                "role": match.group(4),
            }
        )

    if not tables:
        raise RuntimeError("failed to parse result tables from benchmark output")

    return tables


def save_raw_output(text, out_dir, core, rounds):
    out_dir.mkdir(parents=True, exist_ok=True)
    raw_path = out_dir / f"kernel-stride-pf-core{core}-rounds{rounds}.txt"
    raw_path.write_text(text, encoding="utf-8")
    return raw_path


def estimate_hit_threshold_cycles(tables):
    control_rows = []
    all_rows = []

    for title, rows in tables.items():
        all_rows.extend(rows)
        if title.startswith("control:"):
            control_rows.extend(rows)

    rows_for_hit = control_rows if control_rows else all_rows
    hit_values = [
        row["cycles"]
        for row in rows_for_hit
        if row["role"] == "train_input" and row["cycles"] > 0
    ]
    miss_values = [
        row["cycles"]
        for row in rows_for_hit
        if row["role"] == "probe" and row["cycles"] > 0
    ]

    if not hit_values or not miss_values:
        raise RuntimeError("failed to estimate hit threshold from benchmark output")

    hit_median = statistics.median(hit_values)
    miss_median = statistics.median(miss_values)
    if hit_median >= miss_median:
        raise RuntimeError(
            "failed to estimate hit threshold: train_input is not faster than probe"
        )

    return int(round((hit_median + miss_median) / 2))


def classify_bar(row, hit_threshold_cycles):
    if row["role"] == "train_input":
        return "accessed"
    if row["cycles"] < hit_threshold_cycles:
        return "prefetched"
    return "cache_miss"


def plot_tables(tables, out_dir, core, rounds, hit_threshold_cycles):
    os.environ.setdefault("MPLCONFIGDIR", "/tmp/matplotlib-cache")

    try:
        import matplotlib.pyplot as plt
        from matplotlib.patches import Patch
    except ImportError as exc:
        raise RuntimeError(
            "matplotlib is required for plotting. Try: python3 -m pip install matplotlib"
        ) from exc

    class_colors = {
        "accessed": "#D55E00",
        "prefetched": "#0072B2",
        "cache_miss": "#8A8F98",
    }

    fig, axes = plt.subplots(
        len(tables),
        1,
        figsize=(18, 4.8 * len(tables)),
        sharex=True,
        constrained_layout=True,
    )
    if len(tables) == 1:
        axes = [axes]

    for ax, (title, rows) in zip(axes, tables.items()):
        lines = [row["line"] for row in rows]
        values = [row["cycles"] for row in rows]
        classes = [classify_bar(row, hit_threshold_cycles) for row in rows]
        colors = [class_colors[class_name] for class_name in classes]

        ax.bar(lines, values, color=colors, edgecolor="black", linewidth=0.2, width=0.85)
        ax.set_title(title)
        ax.set_ylabel("avg cycles")
        ax.set_ylim(0, 20)
        ax.grid(axis="y", alpha=0.25)

    axes[-1].set_xlabel("kernel array2 cache line")
    axes[-1].set_xlim(-1, 200)

    legend = [
        Patch(facecolor=class_colors["accessed"], edgecolor="black", label="accessed"),
        Patch(facecolor=class_colors["prefetched"], edgecolor="black", label="prefetched"),
        Patch(facecolor=class_colors["cache_miss"], edgecolor="black", label="cache miss"),
    ]
    fig.legend(handles=legend, loc="upper right")
    fig.suptitle(
        f"kernel stride prefetch probe, core={core}, rounds={rounds}, "
        f"hit_threshold={hit_threshold_cycles} cycles"
    )

    out_dir.mkdir(parents=True, exist_ok=True)
    png_path = out_dir / f"kernel-stride-pf-core{core}-rounds{rounds}.png"
    fig.savefig(png_path, dpi=180)
    plt.close(fig)
    return png_path


def main():
    parser = argparse.ArgumentParser(
        description="Build, load, run, plot, and unload the kernel stride prefetch probe."
    )
    parser.add_argument("-c", "--core", type=int, default=0, help="CPU core for taskset")
    parser.add_argument("-r", "--rounds", type=int, default=4000, help="benchmark rounds")
    parser.add_argument("--out-dir", type=Path, default=SCRIPT_DIR / "res", help="output directory")
    parser.add_argument("--skip-build", action="store_true", help="skip `make` before running")
    parser.add_argument("--keep-build", action="store_true", help="do not run `make clean` at the end")
    parser.add_argument("--keep-module", action="store_true", help="do not unload module at the end")
    parser.add_argument(
        "--hit-threshold-cycles",
        type=int,
        default=None,
        help="override the auto-estimated latency threshold for prefetched coloring",
    )
    parser.add_argument(
        "--sudo",
        default="sudo",
        help="command used for privileged insmod/rmmod; set to empty string if already root",
    )
    args = parser.parse_args()

    sudo_cmd = [args.sudo] if args.sudo else []
    module_loaded = False

    try:
        if not args.skip_build:
            build_targets()

        unload_module(sudo_cmd, quiet=True)
        load_module(sudo_cmd)
        module_loaded = True

        output = run_test(args.core, args.rounds)
        print(output, end="")

        raw_path = save_raw_output(output, args.out_dir, args.core, args.rounds)
        tables = parse_tables(output)
        hit_threshold_cycles = args.hit_threshold_cycles
        if hit_threshold_cycles is None:
            hit_threshold_cycles = estimate_hit_threshold_cycles(tables)
            print(f"# auto hit threshold: {hit_threshold_cycles} cycles")
        else:
            print(f"# manual hit threshold: {hit_threshold_cycles} cycles")

        png_path = plot_tables(
            tables,
            args.out_dir,
            args.core,
            args.rounds,
            hit_threshold_cycles,
        )

        print(f"# raw output: {raw_path}")
        print(f"# plot: {png_path}")
    finally:
        if module_loaded and not args.keep_module:
            unload_module(sudo_cmd, quiet=False)
        if not args.keep_build:
            clean_targets()


if __name__ == "__main__":
    main()
