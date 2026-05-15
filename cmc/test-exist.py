#!/usr/bin/env python3

import argparse
import os
import platform
import shlex
import shutil
import subprocess
import sys
from datetime import datetime
from pathlib import Path


HERE = Path(__file__).resolve().parent
SRC = HERE / "test-exist.c"
BIN_DIR = HERE / "bin"
BIN = BIN_DIR / "test-exist"
RES_DIR = HERE / "res"


def run(cmd, capture=False):
    printable = " ".join(str(x) for x in cmd)
    print("+", printable)
    return subprocess.run(cmd, check=True, capture_output=capture, text=True)


def default_compiler():
    machine = platform.machine().lower()
    env_cc = os.environ.get("CC")
    if env_cc:
        return env_cc
    if machine in ("aarch64", "arm64"):
        return "gcc"
    if shutil.which("aarch64-linux-gnu-gcc"):
        return "aarch64-linux-gnu-gcc"
    return "gcc"


def compile_test(compiler, static):
    BIN_DIR.mkdir(exist_ok=True)
    cmd = [
        compiler,
        "-x",
        "c",
        "-std=gnu11",
        "-O0",
        "-Wall",
        "-Wextra",
    ]
    if static:
        cmd.append("-static")
    cmd.extend(["-o", BIN, SRC])
    run(cmd)


def is_native_aarch64():
    return platform.machine().lower() in ("aarch64", "arm64")


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
        if len(parts) != 6 or not parts[0].isdigit():
            continue

        sections[current].append({
            "idx": int(parts[0]),
            "page": int(parts[1]),
            "line": int(parts[2]),
            "role": parts[3],
            "hits": int(parts[4]),
            "per_1000": int(parts[5]),
        })

    return sections


def summarize(output):
    sections = parse_result(output)
    chain = sections.get("depth_next", [])
    controls = sections.get("controls", [])
    chain_avg = sum(row["per_1000"] for row in chain) / len(chain) if chain else 0.0
    control_avg = sum(row["per_1000"] for row in controls) / len(controls) if controls else 0.0
    chain_max = max((row["per_1000"] for row in chain), default=0)
    control_max = max((row["per_1000"] for row in controls), default=0)

    return {
        "chain_avg": chain_avg,
        "control_avg": control_avg,
        "chain_minus_control": chain_avg - control_avg,
        "chain_max": chain_max,
        "control_max": control_max,
    }


def plot_result(output, path, title):
    try:
        os.environ.setdefault("MPLCONFIGDIR", "/tmp/matplotlib-cache")
        import matplotlib.pyplot as plt
    except ImportError:
        print("matplotlib is not available; skip plotting.", file=sys.stderr)
        return False

    sections = parse_result(output)
    chain = sections.get("depth_next", [])
    controls = sections.get("controls", [])
    if not chain and not controls:
        print("No plottable data found.", file=sys.stderr)
        return False

    fig, axes = plt.subplots(2, 1, figsize=(8.0, 5.2), constrained_layout=True)

    for ax, name, rows, color in [
        (axes[0], "Next node after executed pointer-chain depth", chain, "#0072B2"),
        (axes[1], "Controls", controls, "#B8BCC2"),
    ]:
        x = [row["idx"] for row in rows]
        y = [row["per_1000"] for row in rows]
        labels = [f"p{row['page']}:l{row['line']}" for row in rows]
        ax.bar(x, y, color=color, edgecolor="black", linewidth=0.25, width=0.82)
        ax.set_title(name, loc="left", pad=4)
        ax.set_ylabel("Hit rate\n(per 1000 probes)")
        ax.set_ylim(0, 1050)
        ax.set_yticks([0, 250, 500, 750, 1000])
        ax.grid(axis="y", color="#D9D9D9", linewidth=0.7)
        ax.set_axisbelow(True)
        ax.spines["top"].set_visible(False)
        ax.spines["right"].set_visible(False)
        ax.set_xticks(x)
        ax.set_xticklabels(labels, rotation=45, ha="right")

    axes[1].set_xlabel("Probe target")
    fig.suptitle(title, fontsize=12, fontweight="bold")
    path.parent.mkdir(parents=True, exist_ok=True)
    fig.savefig(path, dpi=300)
    plt.close(fig)
    return True


def build_run_command(args):
    test_args = [
        str(BIN),
        str(args.rounds),
        str(args.threshold_ns),
        str(args.training_replays),
        args.access,
    ]

    if args.runner:
        return shlex.split(args.runner) + test_args

    if not is_native_aarch64() and not args.force_run:
        raise SystemExit(
            "This host is not AArch64. Re-run with --build-only, or pass "
            "--runner 'qemu-aarch64 ...' / --force-run if you know the binary is runnable."
        )

    return ["taskset", "-c", str(args.core)] + test_args


def main():
    parser = argparse.ArgumentParser(
        description="Build and run the CMC existence test."
    )
    parser.add_argument("--rounds", type=int, default=40000)
    parser.add_argument("--threshold-ns", type=int, default=150)
    parser.add_argument("--training-replays", type=int, default=128)
    parser.add_argument("--core", type=int, default=5)
    parser.add_argument("--access", choices=["load", "sw"], default="load")
    parser.add_argument("--compiler", default=default_compiler())
    parser.add_argument("--runner", default=None,
                        help="Optional runner prefix, for example: qemu-aarch64 -L /usr/aarch64-linux-gnu")
    parser.add_argument("--output", default=None)
    parser.add_argument("--plot", default=None)
    parser.add_argument("--no-plot", action="store_true")
    parser.add_argument("--no-static", action="store_true")
    parser.add_argument("--no-build", action="store_true")
    parser.add_argument("--build-only", action="store_true")
    parser.add_argument("--force-run", action="store_true")
    args = parser.parse_args()

    if args.rounds <= 0 or args.threshold_ns <= 0 or args.training_replays <= 0:
        raise SystemExit("rounds, threshold-ns, and training-replays must be positive")

    if not args.no_build:
        compile_test(args.compiler, static=not args.no_static)

    if args.build_only:
        return 0

    if not BIN.exists():
        raise SystemExit(f"binary not found: {BIN}")

    RES_DIR.mkdir(exist_ok=True)
    timestamp = datetime.now().strftime("%Y%m%d-%H%M%S")
    result_path = Path(args.output) if args.output else RES_DIR / f"exist-{args.access}-{timestamp}.txt"
    plot_path = Path(args.plot) if args.plot else RES_DIR / f"exist-{args.access}-{timestamp}.png"

    cmd = build_run_command(args)
    env = os.environ.copy()
    env.setdefault("LC_ALL", "C")
    result = subprocess.run(cmd, check=True, env=env, capture_output=True, text=True)

    if result.stdout:
        print(result.stdout, end="")
    if result.stderr:
        print(result.stderr, end="", file=sys.stderr)

    result_path.parent.mkdir(parents=True, exist_ok=True)
    result_path.write_text(result.stdout)
    print(f"Saved result to {result_path}")

    summary = summarize(result.stdout)
    print(
        "Summary: "
        f"chain_avg={summary['chain_avg']:.2f}/1000, "
        f"control_avg={summary['control_avg']:.2f}/1000, "
        f"delta={summary['chain_minus_control']:.2f}/1000, "
        f"chain_max={summary['chain_max']}/1000, "
        f"control_max={summary['control_max']}/1000"
    )

    if not args.no_plot:
        plotted = plot_result(result.stdout, plot_path, "CMC existence test")
        if plotted:
            print(f"Saved plot to {plot_path}")

    return 0


if __name__ == "__main__":
    sys.exit(main())
