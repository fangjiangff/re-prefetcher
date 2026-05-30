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

ARCHES = ["A78", "A55", "A725", "X925"]
CORES = [4, 1, 4, 6]


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


def core_for_arch(arch):
    arch = arch.upper()
    for idx, name in enumerate(ARCHES):
        if name.upper() == arch:
            return CORES[idx]
    supported = ", ".join(ARCHES)
    raise SystemExit(f"unsupported arch: {arch}; supported arches: {supported}")


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
        if current == "node_next":
            if len(parts) != 9 or not parts[0].isdigit():
                continue

            sections[current].append({
                "idx": int(parts[0]),
                "next": int(parts[1]),
                "page": int(parts[2]),
                "line": int(parts[3]),
                "role": parts[4],
                "probes": int(parts[5]),
                "hits": int(parts[6]),
                "per_1000": int(parts[7]),
                "avg_latency_ns": int(parts[8]),
            })
            continue

        if current == "depth_next":
            if not parts[0].isdigit():
                continue

            if len(parts) == 7:
                sections[current].append({
                    "idx": int(parts[0]),
                    "loads": int(parts[1]),
                    "role": parts[2],
                    "probes": int(parts[3]),
                    "hits": int(parts[4]),
                    "per_1000": int(parts[5]),
                    "avg_latency_ns": int(parts[6]),
                })
            elif len(parts) == 8:
                sections[current].append({
                    "idx": int(parts[0]),
                    "page": int(parts[1]),
                    "line": int(parts[2]),
                    "role": parts[3],
                    "probes": int(parts[4]),
                    "hits": int(parts[5]),
                    "per_1000": int(parts[6]),
                    "avg_latency_ns": int(parts[7]),
                })
            continue

        if current == "controls":
            if len(parts) != 6 or not parts[0].isdigit():
                continue

            sections[current].append({
                "idx": int(parts[0]),
                "page": int(parts[1]),
                "line": int(parts[2]),
                "role": parts[3],
                "probes": int(parts[4]),
                "avg_latency_ns": int(parts[5]),
            })
            continue

    return sections


def summarize(output):
    sections = parse_result(output)
    chain = sections.get("node_next", sections.get("depth_next", []))
    controls = sections.get("controls", [])
    chain_avg = sum(row["per_1000"] for row in chain) / len(chain) if chain else 0.0
    chain_max = max((row["per_1000"] for row in chain), default=0)
    control_latency_avg = (
        sum(row["avg_latency_ns"] for row in controls) / len(controls)
        if controls else 0.0
    )
    control_latency_min = min((row["avg_latency_ns"] for row in controls), default=0)

    return {
        "chain_avg": chain_avg,
        "chain_max": chain_max,
        "control_latency_avg": control_latency_avg,
        "control_latency_min": control_latency_min,
    }


def plot_result(output, path, title):
    try:
        os.environ.setdefault("MPLCONFIGDIR", "/tmp/matplotlib-cache")
        import matplotlib.pyplot as plt
    except ImportError:
        print("matplotlib is not available; skip plotting.", file=sys.stderr)
        return False

    sections = parse_result(output)
    chain = sections.get("node_next", sections.get("depth_next", []))
    controls = sections.get("controls", [])
    if not chain and not controls:
        print("No plottable data found.", file=sys.stderr)
        return False

    if "node_next" in sections:
        chain_title = "Next node after one trained node access"
    elif chain and chain[0].get("role") == "next_after_window":
        chain_title = "Next node after K predecessor accesses"
    elif chain and chain[0].get("role") == "current_after_window":
        chain_title = "Current node after fixed predecessor window"
    elif chain and chain[0].get("role") == "next_after_direct":
        chain_title = "Next node after direct chain_nodes accesses"
    else:
        chain_title = "Next node after executed pointer-chain depth"

    fig, axes = plt.subplots(2, 1, figsize=(8.0, 5.2), constrained_layout=True)

    for ax, name, rows, color, metric, ylabel in [
        (
            axes[0],
            chain_title,
            chain,
            "#0072B2",
            "per_1000",
            "Hit rate\n(per 1000 probes)",
        ),
        (
            axes[1],
            "Controls",
            controls,
            "#B8BCC2",
            "avg_latency_ns",
            "Average latency\n(ns)",
        ),
    ]:
        x = [row["idx"] for row in rows]
        y = [row[metric] for row in rows]
        labels = [
            f"p{row['page']}:l{row['line']}"
            if "page" in row and "line" in row else
            str(row["idx"])
            for row in rows
        ]
        ax.bar(x, y, color=color, edgecolor="black", linewidth=0.25, width=0.82)
        ax.set_title(name, loc="left", pad=4)
        ax.set_ylabel(ylabel)
        if metric == "per_1000":
            ax.set_ylim(0, 1050)
            ax.set_yticks([0, 250, 500, 750, 1000])
        elif y:
            ax.set_ylim(0, max(y) * 1.15)
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
        args.mode,
    ]
    if args.window_k is not None:
        test_args.append(str(args.window_k))
    test_args.append(args.access)

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
    parser.add_argument("--threshold-ns", type=int, default=120)
    parser.add_argument("--training-replays", type=int, default=10)
    parser.add_argument("--mode", choices=["node", "depth", "direct", "window"], default="node",
                        help="node: access node[n] then probe node[n+1]; depth: pointer-load node0..node(depth-1) then probe node(depth); direct: directly load chain_nodes[0..depth-1] then probe chain_nodes[depth]; window: sweep K by default, or use --window-k to probe current node after K predecessors.")
    parser.add_argument("--window-k", type=int, default=None,
                        help="For --mode window, use fixed K: access node[n-K]..node[n-1], then probe node[n]. Omit to sweep K.")
    parser.add_argument("-a", "--arch", type=str.upper, choices=ARCHES, default="A725")
    parser.add_argument("-c", "--core", type=int, default=None,
                        help="Override the core selected by --arch.")
    parser.add_argument("--access", choices=["load", "sw"], default="load",
                        help="load: normal load accesses; sw: direct-mode PRFM accesses only.")
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

    if len(ARCHES) != len(CORES):
        raise SystemExit("ARCHES and CORES must have the same length")
    if args.core is None:
        args.core = core_for_arch(args.arch)

    if args.rounds <= 0 or args.threshold_ns <= 0 or args.training_replays <= 0:
        raise SystemExit("rounds, threshold-ns, and training-replays must be positive")
    if args.window_k is not None:
        if args.mode != "window":
            raise SystemExit("--window-k can only be used with --mode window")
        if args.window_k < 0 or args.window_k >= 128:
            raise SystemExit("--window-k must be in the range 0..127")
    if args.access == "sw" and args.mode != "direct":
        raise SystemExit("--access sw is only supported with --mode direct")

    if not args.no_build:
        compile_test(args.compiler, static=not args.no_static)

    if args.build_only:
        return 0

    if not BIN.exists():
        raise SystemExit(f"binary not found: {BIN}")

    RES_DIR.mkdir(exist_ok=True)
    timestamp = datetime.now().strftime("%Y%m%d-%H%M%S")
    window_suffix = f"-wink{args.window_k}" if args.window_k is not None else ""
    stem = (
        f"exist-{args.access}-{args.arch}-cpu{args.core}-"
        f"{args.mode}{window_suffix}-thr{args.threshold_ns}-{timestamp}"
    )
    result_path = Path(args.output) if args.output else RES_DIR / f"{stem}.txt"
    plot_path = Path(args.plot) if args.plot else RES_DIR / f"{stem}.png"

    cmd = build_run_command(args)
    env = os.environ.copy()
    env.setdefault("LC_ALL", "C")
    result = subprocess.run(cmd, env=env, capture_output=True, text=True)

    if result.stdout:
        print(result.stdout, end="")
    if result.stderr:
        print(result.stderr, end="", file=sys.stderr)
    if result.returncode != 0:
        return result.returncode

    result_path.parent.mkdir(parents=True, exist_ok=True)
    result_path.write_text(result.stdout)
    print(f"Saved result to {result_path}")

    summary = summarize(result.stdout)
    print(
        "Summary: "
        f"chain_avg={summary['chain_avg']:.2f}/1000, "
        f"chain_max={summary['chain_max']}/1000, "
        f"control_latency_avg={summary['control_latency_avg']:.2f} ns, "
        f"control_latency_min={summary['control_latency_min']} ns"
    )

    if not args.no_plot:
        plotted = plot_result(
            result.stdout,
            plot_path,
            f"CMC existence test {args.arch} CPU {args.core} {args.mode} threshold {args.threshold_ns} ns",
        )
        if plotted:
            print(f"Saved plot to {plot_path}")

    return 0


if __name__ == "__main__":
    sys.exit(main())
