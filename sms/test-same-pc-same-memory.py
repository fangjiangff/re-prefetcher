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
SRC = HERE / "test-same-pc-same-memory.c"
BIN_DIR = HERE / "bin"
BIN = BIN_DIR / "test-same-pc-same-memory"
RES_DIR = HERE / "res"


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


def plot_result(output, path):
    sections = parse_result(output)
    if not sections:
        print("No plottable data found.", file=sys.stderr)
        return

    names = list(sections.keys())
    fig, axes = plt.subplots(len(names), 1, figsize=(14, 4 * len(names)), sharex=True)
    if len(names) == 1:
        axes = [axes]

    for ax, name in zip(axes, names):
        rows = sections[name]
        lines = [row["line"] for row in rows]
        values = [row["per_1000"] for row in rows]
        colors = []
        for row in rows:
            if row["role"] == "trigger":
                colors.append("#d62728")
            elif row["role"] == "expected_prefetch":
                colors.append("#2ca02c")
            else:
                colors.append("#4c78a8")

        ax.bar(lines, values, color=colors, width=0.85)
        ax.set_title(name)
        ax.set_ylabel("hits per 1000 probes")
        ax.set_ylim(bottom=0)
        ax.grid(axis="y", alpha=0.3)

    axes[-1].set_xlabel("cache line in test page")
    fig.tight_layout()
    fig.savefig(path, dpi=200)
    plt.close(fig)


def main():
    parser = argparse.ArgumentParser(
        description="Build and run the SMS same-PC same-memory test."
    )
    parser.add_argument("--rounds", type=int, default=40000)
    parser.add_argument("--threshold-ns", type=int, default=150)
    parser.add_argument("--core", type=int, default=0)
    parser.add_argument("--access", choices=["load", "sw"], default="load")
    parser.add_argument("--output", default=None)
    parser.add_argument("--plot", default=None)
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
            f"-DUSE_SW_PREFETCH={1 if args.access == 'sw' else 0}",
            "-o",
            BIN,
            SRC,
        ])

    if args.build_only:
        return 0

    if not BIN.exists():
        raise SystemExit(f"binary not found: {BIN}")

    cmd = [
        "taskset",
        "-c",
        str(args.core),
        BIN,
        str(args.rounds),
        str(args.threshold_ns),
    ]

    env = os.environ.copy()
    env.setdefault("LC_ALL", "C")
    print("+", " ".join(str(x) for x in cmd))
    result = subprocess.run(cmd, check=True, env=env, capture_output=True, text=True)

    if result.stdout:
        print(result.stdout, end="")
    if result.stderr:
        print(result.stderr, end="", file=sys.stderr)

    RES_DIR.mkdir(exist_ok=True)
    timestamp = datetime.now().strftime("%Y%m%d-%H%M%S")
    result_path = Path(args.output) if args.output else RES_DIR / f"same-pc-same-memory-{args.access}-{timestamp}.txt"
    plot_path = Path(args.plot) if args.plot else RES_DIR / f"same-pc-same-memory-{args.access}-{timestamp}.png"

    result_path.parent.mkdir(parents=True, exist_ok=True)
    result_path.write_text(result.stdout)
    print(f"Saved result to {result_path}")

    plot_path.parent.mkdir(parents=True, exist_ok=True)
    plot_result(result.stdout, plot_path)
    print(f"Saved plot to {plot_path}")

    return 0


if __name__ == "__main__":
    sys.exit(main())
