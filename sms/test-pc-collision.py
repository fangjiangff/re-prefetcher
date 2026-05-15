#!/usr/bin/env python3

import argparse
import os
import subprocess
import sys
from datetime import datetime
from pathlib import Path

os.environ.setdefault("MPLCONFIGDIR", "/tmp/matplotlib-cache")
import matplotlib.pyplot as plt


HERE = Path(__file__).resolve().parent
SRC = HERE / "test-pc-collision.c"
BIN_DIR = HERE / "bin"
BIN = BIN_DIR / "test-pc-collision"
RES_DIR = HERE / "res"
DEFAULT_ARCH = "CortexA76"

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
})


def run(cmd):
    print("+", " ".join(str(x) for x in cmd))
    return subprocess.run(cmd, check=True, capture_output=True, text=True)


def compile_test():
    BIN_DIR.mkdir(exist_ok=True)
    run([
        "g++",
        "-x",
        "c",
        "-std=gnu11",
        "-O0",
        "-Wall",
        "-Wextra",
        "-o",
        BIN,
        SRC,
    ])


def parse_result(output):
    rows = []
    metadata = {}

    for line in output.splitlines():
        stripped = line.strip()
        if not stripped:
            continue
        if stripped.startswith("# train_pc="):
            for item in stripped[2:].split():
                if "=" in item:
                    key, value = item.split("=", 1)
                    metadata[key] = value
            continue
        if stripped.startswith("#"):
            continue

        parts = stripped.split()
        if len(parts) != 4 or not parts[0].isdigit():
            continue

        rows.append({
            "line": int(parts[0]),
            "role": parts[1],
            "hits": int(parts[2]),
            "per_1000": int(parts[3]),
        })

    return metadata, rows


def summarize_output(mode, bits, output, prefetch_min):
    metadata, rows = parse_result(output)
    expected = [row for row in rows if row["role"] == "expected_prefetch"]
    trigger = next((row for row in rows if row["role"] == "trigger"), None)
    detected = [row for row in expected if row["per_1000"] >= prefetch_min]

    avg_expected = 0.0
    if expected:
        avg_expected = sum(row["per_1000"] for row in expected) / len(expected)

    return {
        "mode": mode,
        "colliding_bits": bits,
        "detected_prefetch_lines": len(detected),
        "expected_lines": len(expected),
        "avg_expected_per_1000": avg_expected,
        "trigger_per_1000": trigger["per_1000"] if trigger else 0,
        "pc_lsb_equal": int(metadata.get("pc_lsb_equal", 0)),
        "train_pc": metadata.get("train_pc", ""),
        "trigger_pc": metadata.get("trigger_pc", ""),
    }


def save_summary(path, summaries):
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w") as f:
        f.write(
            "mode\tcolliding_bits\tdetected_prefetch_lines\texpected_lines\t"
            "avg_expected_per_1000\ttrigger_per_1000\tpc_lsb_equal\ttrain_pc\ttrigger_pc\n"
        )
        for row in summaries:
            f.write(
                f"{row['mode']}\t{row['colliding_bits']}\t{row['detected_prefetch_lines']}\t"
                f"{row['expected_lines']}\t{row['avg_expected_per_1000']:.2f}\t"
                f"{row['trigger_per_1000']}\t{row['pc_lsb_equal']}\t"
                f"{row['train_pc']}\t{row['trigger_pc']}\n"
            )


def plot_summary(path, summaries, title):
    by_mode = {}
    for row in summaries:
        by_mode.setdefault(row["mode"], []).append(row)

    fig, axes = plt.subplots(2, 1, figsize=(7.2, 5.2), sharex=True)
    colors = {
        "load": "#D55E00",
        "sw": "#0072B2",
    }
    labels = {
        "load": "load",
        "sw": "software prefetch",
    }

    for mode, rows in by_mode.items():
        rows = sorted(rows, key=lambda row: row["colliding_bits"])
        x = [row["colliding_bits"] for row in rows]
        avg = [row["avg_expected_per_1000"] for row in rows]
        count = [row["detected_prefetch_lines"] for row in rows]
        color = colors.get(mode, "#4C78A8")
        label = labels.get(mode, mode)

        axes[0].plot(x, avg, marker="o", linewidth=1.8, color=color, label=label)
        axes[1].plot(x, count, marker="o", linewidth=1.8, color=color, label=label)

    axes[0].set_title("Average expected-line hit rate", loc="left", pad=4)
    axes[0].set_ylabel("Hit rate\n(per 1000 probes)")
    axes[0].set_ylim(0, 1050)
    axes[0].set_yticks([0, 250, 500, 750, 1000])

    axes[1].set_title("Detected expected prefetch lines", loc="left", pad=4)
    axes[1].set_ylabel("Lines")
    axes[1].set_xlabel("Colliding low-order PC bits")
    axes[1].set_ylim(-0.2, 3.2)
    axes[1].set_yticks([0, 1, 2, 3])

    for ax in axes:
        ax.grid(axis="y", color="#D9D9D9", linewidth=0.7)
        ax.set_axisbelow(True)
        ax.spines["top"].set_visible(False)
        ax.spines["right"].set_visible(False)
        ax.tick_params(axis="both", length=3, width=0.8)

    axes[0].legend(loc="upper right", frameon=False)
    fig.suptitle(title, fontsize=12, fontweight="bold", y=0.965)
    fig.tight_layout(rect=(0, 0, 1, 0.955))
    fig.savefig(path)
    plt.close(fig)


def main():
    parser = argparse.ArgumentParser(
        description="Build and run the SMS PC-collision test."
    )
    parser.add_argument("--rounds", type=int, default=40000)
    parser.add_argument("--threshold-ns", type=int, default=150)
    parser.add_argument("--core", type=int, default=0)
    parser.add_argument("--min-bits", type=int, default=5)
    parser.add_argument("--max-bits", type=int, default=24)
    parser.add_argument("--prefetch-min", type=int, default=100)
    parser.add_argument("--arch", default=DEFAULT_ARCH)
    parser.add_argument("--access", choices=["both", "load", "sw"], default="both")
    parser.add_argument("--output-prefix", default=None)
    parser.add_argument("--no-build", action="store_true")
    parser.add_argument("--build-only", action="store_true")
    args = parser.parse_args()

    if args.min_bits < 5 or args.max_bits > 24 or args.max_bits < args.min_bits:
        raise SystemExit("colliding bits range must be within [5, 24]")

    if not args.no_build:
        compile_test()

    if args.build_only:
        return 0

    if not BIN.exists():
        raise SystemExit(f"binary not found: {BIN}")

    RES_DIR.mkdir(exist_ok=True)
    timestamp = datetime.now().strftime("%Y%m%d-%H%M%S")
    prefix = args.output_prefix or f"{args.arch}-pc-collision-{timestamp}"
    modes = ["load", "sw"] if args.access == "both" else [args.access]

    summaries = []
    for mode in modes:
        raw_path = RES_DIR / f"{prefix}-{mode}.txt"
        with raw_path.open("w") as raw:
            for bits in range(args.min_bits, args.max_bits + 1):
                cmd = [
                    "taskset",
                    "-c",
                    str(args.core),
                    BIN,
                    str(args.rounds),
                    str(args.threshold_ns),
                    str(bits),
                    mode,
                ]
                result = run(cmd)
                if result.stdout:
                    print(result.stdout, end="")
                    raw.write(f"\n===== mode={mode} colliding_bits={bits} =====\n")
                    raw.write(result.stdout)
                if result.stderr:
                    print(result.stderr, end="", file=sys.stderr)

                summaries.append(summarize_output(mode, bits, result.stdout, args.prefetch_min))
        print(f"Saved raw result to {raw_path}")

    summary_path = RES_DIR / f"{prefix}-summary.tsv"
    save_summary(summary_path, summaries)
    print(f"Saved summary to {summary_path}")

    plot_path = RES_DIR / f"{prefix}-combined.png"
    plot_summary(plot_path, summaries, f"test-pc-collision [{args.arch}] (load vs software prefetch)")
    print(f"Saved plot to {plot_path}")

    return 0


if __name__ == "__main__":
    sys.exit(main())
