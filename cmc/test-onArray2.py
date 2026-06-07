#!/usr/bin/env python3

import argparse
import os
import platform
import signal
import shlex
import shutil
import subprocess
import sys
from pathlib import Path


HERE = Path(__file__).resolve().parent
SRC = HERE / "test-onArray2.c"
BIN_DIR = HERE / "bin"
BIN = BIN_DIR / "test-onArray2"
RES_DIR = HERE / "res"

ARCHES = ["A78", "A55", "A725", "X925"]
CORES = [4, 1, 4, 6]
LINE_SIZE = 64
ARRAY_INDEX_COUNT = 256


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


def is_native_aarch64():
    return platform.machine().lower() in ("aarch64", "arm64")


def core_for_arch(arch):
    arch = arch.upper()
    for idx, name in enumerate(ARCHES):
        if name.upper() == arch:
            return CORES[idx]
    supported = ", ".join(ARCHES)
    raise SystemExit(f"unsupported arch: {arch}; supported arches: {supported}")


def compile_test(args):
    BIN_DIR.mkdir(exist_ok=True)
    cmd = [
        args.compiler,
        "-x",
        "c",
        "-std=gnu11",
        "-O0",
        "-Wall",
        "-Wextra",
        f"-DROUNDS={args.rounds}",
        f"-DPROBE_POSITIONS={args.probe_positions}",
        f"-DDEFAULT_PROBE_INDEX={args.trigger_index}",
        f"-DTRIGGER_START={args.trigger_start}",
        f"-DTRIGGER_END={args.trigger_end}",
        f"-DItems={args.items}",
        f"-DRANDOM_BUFFER_PAGES={args.random_pages}",
        f"-DRANDOM_ACCESS_PCS={args.random_pcs}",
        f"-DENABLE_CONTEXT_SWITCH_FLUSH={1 if args.context_switch_flush else 0}",
    ]
    if args.static:
        cmd.append("-static")
    cmd.extend(["-o", BIN, SRC])
    run(cmd)


def parse_result(output):
    rows = []

    for line in output.splitlines():
        line = line.strip()
        if not line or line.startswith("#"):
            continue

        parts = line.split()
        if len(parts) != 4 or not parts[0].isdigit():
            continue

        rows.append({
            "position": int(parts[0]),
            "offset_bytes": int(parts[1]),
            "avg_ns": int(parts[2]),
            "probes": int(parts[3]),
        })

    return rows


def summarize(output):
    rows = parse_result(output)
    if not rows:
        return {
            "count": 0,
            "avg_ns": 0.0,
            "min_ns": 0,
            "max_ns": 0,
            "fastest": [],
        }

    latencies = [row["avg_ns"] for row in rows]
    fastest = sorted(rows, key=lambda row: row["avg_ns"])[:8]
    return {
        "count": len(rows),
        "avg_ns": sum(latencies) / len(latencies),
        "min_ns": min(latencies),
        "max_ns": max(latencies),
        "fastest": fastest,
    }


def plot_result(output, path, title, trigger_index):
    try:
        os.environ.setdefault("MPLCONFIGDIR", "/tmp/matplotlib-cache")
        import matplotlib.pyplot as plt
    except ImportError:
        print("matplotlib is not available; skip plotting.", file=sys.stderr)
        return False

    rows = parse_result(output)
    if not rows:
        print("No plottable data found.", file=sys.stderr)
        return False

    positions = [row["position"] for row in rows]
    latencies = [row["avg_ns"] for row in rows]
    trigger_position = trigger_index

    colors = []
    for pos in positions:
        if pos == trigger_position:
            colors.append("#D55E00")
        else:
            colors.append("#B8BCC2")

    fig, ax = plt.subplots(figsize=(10.0, 4.8), constrained_layout=True)
    ax.bar(positions, latencies, color=colors, edgecolor="black", linewidth=0.2, width=0.82)
    ax.set_title(title, loc="left", pad=4)
    ax.set_xlabel("Probe position")
    ax.set_ylabel("Average latency (ns)")
    ax.grid(axis="y", color="#D9D9D9", linewidth=0.7)
    ax.set_axisbelow(True)
    ax.spines["top"].set_visible(False)
    ax.spines["right"].set_visible(False)

    if positions:
        step = 16 if max(positions) >= 128 else 8
        ax.set_xticks(list(range(0, max(positions) + 1, step)))

    path.parent.mkdir(parents=True, exist_ok=True)
    fig.savefig(path, dpi=300)
    plt.close(fig)
    return True


def build_run_command(args):
    test_args = [str(BIN)]

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
        description="Build and run the array2 CMC irregular-load latency map."
    )
    parser.add_argument("--rounds", type=int, default=8000)
    parser.add_argument("--probe-positions", type=int, default=2000)
    parser.add_argument("--trigger-index", type=int, default=0,
                        help="Index into the fixed 16-entry training sequence.")
    parser.add_argument("--trigger-start", type=int, default=170,
                        help="First array_index slot directly loaded in the trigger phase.")
    parser.add_argument("--trigger-end", type=int, default=195,
                        help="One-past-last array_index slot directly loaded in the trigger phase.")
    parser.add_argument("--items", type=int, default=10240)
    parser.add_argument("--random-pages", type=int, default=1024,
                        help="Number of 4 KiB pages used by randomAccesses.")
    parser.add_argument("--random-pcs", type=int, default=8,
                        help="Number of distinct load PCs used by randomAccesses, 1..8.")
    parser.add_argument("--context-switch-flush", action="store_true",
                        help="Force a same-core process context switch at the start of every attack round.")
    parser.add_argument("-a", "--arch", type=str.upper, choices=ARCHES, default="A725")
    parser.add_argument("-c", "--core", type=int, default=None,
                        help="Override the core selected by --arch.")
    parser.add_argument("--compiler", default=default_compiler())
    parser.add_argument("--runner", default=None,
                        help="Optional runner prefix, for example: qemu-aarch64 -L /usr/aarch64-linux-gnu")
    parser.add_argument("--output", default=None)
    parser.add_argument("--plot", default=None)
    parser.add_argument("--no-plot", action="store_true")
    parser.add_argument("--static", action="store_true", default=True)
    parser.add_argument("--no-static", action="store_false", dest="static")
    parser.add_argument("--no-build", action="store_true")
    parser.add_argument("--build-only", action="store_true")
    parser.add_argument("--force-run", action="store_true")
    args = parser.parse_args()

    if len(ARCHES) != len(CORES):
        raise SystemExit("ARCHES and CORES must have the same length")
    if args.core is None:
        args.core = core_for_arch(args.arch)

    if args.rounds <= 0 or args.probe_positions <= 0 or args.items <= 0:
        raise SystemExit("rounds, probe-positions, and items must be positive")
    if args.random_pages <= 0:
        raise SystemExit("--random-pages must be positive")
    if args.random_pcs < 1 or args.random_pcs > 8:
        raise SystemExit("--random-pcs must be in the range 1..8")
    if args.trigger_index < 0 or args.trigger_index >= ARRAY_INDEX_COUNT:
        raise SystemExit(f"--trigger-index must be in the range 0..{ARRAY_INDEX_COUNT - 1}")
    if args.trigger_start < 0 or args.trigger_end <= args.trigger_start or args.trigger_end > 256:
        raise SystemExit("--trigger-start/--trigger-end must describe a non-empty range within 0..256")
    if args.probe_positions > args.items:
        raise SystemExit("--probe-positions must be <= --items")

    if not args.no_build:
        compile_test(args)

    if args.build_only:
        return 0

    if not BIN.exists():
        raise SystemExit(f"binary not found: {BIN}")

    RES_DIR.mkdir(exist_ok=True)
    stem = (
        f"onArray2-load-{args.arch}-cpu{args.core}-"
        f"r{args.rounds}-p{args.probe_positions}-trigidx{args.trigger_index}-"
        f"tw{args.trigger_start}-{args.trigger_end}-"
        f"randpg{args.random_pages}-pc{args.random_pcs}-"
        f"csw{1 if args.context_switch_flush else 0}"
    )
    result_path = Path(args.output) if args.output else RES_DIR / f"{stem}.txt"
    plot_path = Path(args.plot) if args.plot else RES_DIR / f"{stem}.png"

    cmd = build_run_command(args)
    env = os.environ.copy()
    env.setdefault("LC_ALL", "C")
    print("+", " ".join(str(x) for x in cmd))
    result = subprocess.run(cmd, env=env, capture_output=True, text=True)

    if result.stdout:
        print(result.stdout, end="")
    if result.stderr:
        print(result.stderr, end="", file=sys.stderr)
    if result.returncode != 0:
        if result.returncode < 0:
            sig = signal.Signals(-result.returncode).name
            print(
                f"Binary terminated by {sig}.",
                file=sys.stderr,
            )
        return result.returncode

    result_path.parent.mkdir(parents=True, exist_ok=True)
    result_path.write_text(result.stdout)
    print(f"Saved result to {result_path}")

    summary = summarize(result.stdout)
    fastest = ", ".join(
        f"{row['position']}:{row['avg_ns']}ns"
        for row in summary["fastest"]
    )
    print(
        "Summary: "
        f"positions={summary['count']}, "
        f"avg={summary['avg_ns']:.2f} ns, "
        f"min={summary['min_ns']} ns, "
        f"max={summary['max_ns']} ns, "
        f"fastest=[{fastest}]"
    )

    if not args.no_plot:
        plotted = plot_result(
            result.stdout,
            plot_path,
            f"array2 CMC latency map {args.arch} CPU {args.core} trigger index {args.trigger_index}",
            args.trigger_index,
        )
        if plotted:
            print(f"Saved plot to {plot_path}")

    return 0


if __name__ == "__main__":
    sys.exit(main())
