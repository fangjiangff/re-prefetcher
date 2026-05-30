#!/usr/bin/env python3

import argparse
import os
import platform
import re
import shlex
import shutil
import subprocess
import sys
from datetime import datetime
from pathlib import Path


HERE = Path(__file__).resolve().parent
SRC = HERE / "cmc_probe.c"
BIN_DIR = HERE / "bin"
BIN = BIN_DIR / "cmc_probe"
RES_DIR = HERE / "res"

ARCHES = ["A78", "A55", "A725", "X925", "A76"]
CORES = [4, 1, 4, 6, 1]


FIXED_RE = re.compile(r"fixed pass\s+(\d+):\s+([0-9.]+)\s+ns/load")
CTRL_RE = re.compile(r"ctrl\s+pass\s+(\d+):\s+([0-9.]+)\s+ns/load")
SUMMARY_RE = re.compile(r"^\s*([^:]+):\s+([0-9.]+)")
VERDICT_RE = re.compile(r"^\s*verdict:\s*(.*)")


def run(cmd):
    printable = " ".join(str(x) for x in cmd)
    print("+", printable)
    return subprocess.run(cmd, check=True)


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


def compile_probe(compiler, static):
    BIN_DIR.mkdir(exist_ok=True)
    cmd = [
        compiler,
        "-x",
        "c",
        "-std=gnu11",
        "-O2",
        "-march=armv8.2-a",
        "-fno-tree-vectorize",
        "-fno-prefetch-loop-arrays",
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
    fixed = []
    control = []
    summary = {}
    verdict = None

    for line in output.splitlines():
        fixed_match = FIXED_RE.search(line)
        if fixed_match:
            fixed.append({
                "pass": int(fixed_match.group(1)),
                "ns_per_load": float(fixed_match.group(2)),
            })
            continue

        ctrl_match = CTRL_RE.search(line)
        if ctrl_match:
            control.append({
                "pass": int(ctrl_match.group(1)),
                "ns_per_load": float(ctrl_match.group(2)),
            })
            continue

        verdict_match = VERDICT_RE.match(line)
        if verdict_match:
            verdict = verdict_match.group(1)
            continue

        summary_match = SUMMARY_RE.match(line)
        if summary_match:
            key = summary_match.group(1).strip()
            value = float(summary_match.group(2))
            summary[key] = value

    return {
        "fixed": fixed,
        "control": control,
        "summary": summary,
        "verdict": verdict,
    }


def summarize(output):
    parsed = parse_result(output)
    summary = parsed["summary"]
    return {
        "fixed_pass0": summary.get("fixed pass0"),
        "fixed_warm": summary.get("fixed warm avg last half"),
        "control_avg": summary.get("reshuffled control avg"),
        "speedup_pass0": summary.get("warm speedup vs pass0"),
        "speedup_control": summary.get("warm speedup vs control"),
        "verdict": parsed["verdict"],
    }


def plot_result(output, path, title, metric, ylim):
    try:
        os.environ.setdefault("MPLCONFIGDIR", "/tmp/matplotlib-cache")
        import matplotlib.pyplot as plt
    except ImportError:
        print("matplotlib is not available; skip plotting.", file=sys.stderr)
        return False

    parsed = parse_result(output)
    fixed = parsed["fixed"]
    control = parsed["control"]
    if not fixed and not control:
        print("No plottable data found.", file=sys.stderr)
        return False

    if metric == "speedup":
        if not fixed:
            print("No fixed pass data found for speedup baseline.", file=sys.stderr)
            return False
        baseline = fixed[0]["ns_per_load"]

    fig, ax = plt.subplots(figsize=(8.0, 4.5), constrained_layout=True)

    if fixed:
        fixed_y = [row["ns_per_load"] for row in fixed]
        if metric == "speedup":
            fixed_y = [baseline / y for y in fixed_y]
        ax.plot(
            [row["pass"] for row in fixed],
            fixed_y,
            marker="o",
            linewidth=1.8,
            color="#0072B2",
            label="fixed repeated stream",
        )

    if control:
        control_y = [row["ns_per_load"] for row in control]
        if metric == "speedup":
            control_y = [baseline / y for y in control_y]
        ax.plot(
            [row["pass"] for row in control],
            control_y,
            marker="s",
            linewidth=1.8,
            color="#D55E00",
            label="reshuffled control",
        )

    ax.set_title(title, loc="left", pad=6)
    ax.set_xlabel("Pass")
    if metric == "speedup":
        ax.axhline(1.0, color="#666666", linewidth=0.9, linestyle="--")
        ax.set_ylabel("Speedup vs fixed pass 0 (x)")
    else:
        ax.set_ylabel("Latency (ns/load)")
    ax.set_ylim(ylim[0], ylim[1])
    ax.grid(axis="y", color="#D9D9D9", linewidth=0.7)
    ax.set_axisbelow(True)
    ax.spines["top"].set_visible(False)
    ax.spines["right"].set_visible(False)
    ax.legend(frameon=False)

    path.parent.mkdir(parents=True, exist_ok=True)
    fig.savefig(path, dpi=300)
    plt.close(fig)
    return True


def build_run_command(args):
    probe_args = [
        str(BIN),
        "-n",
        str(args.nodes),
        "-s",
        str(args.spacing),
        "-p",
        str(args.passes),
        "-r",
        str(args.seed),
    ]
    if args.pass_core_to_probe:
        probe_args.extend(["-c", str(args.core)])

    if args.runner:
        return shlex.split(args.runner) + probe_args

    if not is_native_aarch64() and not args.force_run:
        raise SystemExit(
            "This host is not AArch64. Re-run with --build-only, or pass "
            "--runner 'qemu-aarch64 ...' / --force-run if you know the binary is runnable."
        )

    if args.no_taskset:
        return probe_args

    return ["taskset", "-c", str(args.core)] + probe_args


def parse_ylim(value):
    try:
        low_text, high_text = value.split(":", 1)
        low = float(low_text)
        high = float(high_text)
    except ValueError as exc:
        raise argparse.ArgumentTypeError("expected LOW:HIGH, for example 0.8:2.2") from exc
    if low >= high:
        raise argparse.ArgumentTypeError("plot y-min must be smaller than y-max")
    return (low, high)


def main():
    parser = argparse.ArgumentParser(
        description="Build, run, save, and optionally plot cmc_probe results."
    )
    parser.add_argument("-n", "--nodes", type=int, default=32768)
    parser.add_argument("-s", "--spacing", type=int, default=4096)
    parser.add_argument("-p", "--passes", type=int, default=8)
    parser.add_argument("-a", "--arch", type=str.upper, choices=ARCHES, default="A76")
    parser.add_argument("-c", "--core", type=int, default=None,
                        help="Override the core selected by --arch.")
    parser.add_argument("-r", "--seed", type=int, default=1)
    parser.add_argument("--compiler", default=default_compiler())
    parser.add_argument("--runner", default=None,
                        help="Optional runner prefix, for example: qemu-aarch64 -L /usr/aarch64-linux-gnu")
    parser.add_argument("--input", default=None,
                        help="Plot/summarize an existing cmc_probe result without running the binary.")
    parser.add_argument("--output", default=None)
    parser.add_argument("--plot", default=None)
    parser.add_argument("--plot-metric", choices=["speedup", "latency"], default="speedup")
    parser.add_argument("--plot-ylim", type=parse_ylim, default=None,
                        help="Fixed plot y-axis range as LOW:HIGH.")
    parser.add_argument("--no-plot", action="store_true")
    parser.add_argument("--no-static", action="store_true")
    parser.add_argument("--no-build", action="store_true")
    parser.add_argument("--build-only", action="store_true")
    parser.add_argument("--force-run", action="store_true")
    parser.add_argument("--no-taskset", action="store_true")
    parser.add_argument("--pass-core-to-probe", action="store_true",
                        help="Pass -c to cmc_probe instead of relying only on taskset.")
    args = parser.parse_args()

    if len(ARCHES) != len(CORES):
        raise SystemExit("ARCHES and CORES must have the same length")
    if args.core is None:
        args.core = core_for_arch(args.arch)

    if args.nodes < 1024 or args.passes < 3:
        raise SystemExit("nodes must be >= 1024 and passes must be >= 3")
    if args.spacing < 8 or args.spacing % 64 != 0:
        raise SystemExit("spacing must be >= 8 and a multiple of 64")

    RES_DIR.mkdir(exist_ok=True)
    timestamp = datetime.now().strftime("%Y%m%d-%H%M%S")
    stem = f"cmc-probe-{args.arch}-cpu{args.core}-n{args.nodes}-s{args.spacing}-{timestamp}"
    result_path = Path(args.output) if args.output else RES_DIR / f"{stem}.txt"
    plot_path = Path(args.plot) if args.plot else RES_DIR / f"{stem}.png"
    plot_ylim = args.plot_ylim
    if plot_ylim is None:
        plot_ylim = (0.8, 2.0) if args.plot_metric == "speedup" else (0.0, 220.0)

    if args.input:
        output = Path(args.input).read_text()
        summary = summarize(output)
        print(
            "Summary: "
            f"fixed_pass0={summary['fixed_pass0']:.2f} ns/load, "
            f"fixed_warm={summary['fixed_warm']:.2f} ns/load, "
            f"control_avg={summary['control_avg']:.2f} ns/load, "
            f"speedup_vs_pass0={summary['speedup_pass0']:.2f}x, "
            f"speedup_vs_control={summary['speedup_control']:.2f}x"
        )
        if summary["verdict"]:
            print(f"Verdict: {summary['verdict']}")
        if not args.no_plot:
            plotted = plot_result(
                output,
                plot_path,
                f"cmc_probe {args.arch} CPU {args.core}",
                args.plot_metric,
                plot_ylim,
            )
            if plotted:
                print(f"Saved plot to {plot_path}")
        return 0

    if not args.no_build:
        compile_probe(args.compiler, static=not args.no_static)

    if args.build_only:
        return 0

    if not BIN.exists():
        raise SystemExit(f"binary not found: {BIN}")

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
        f"fixed_pass0={summary['fixed_pass0']:.2f} ns/load, "
        f"fixed_warm={summary['fixed_warm']:.2f} ns/load, "
        f"control_avg={summary['control_avg']:.2f} ns/load, "
        f"speedup_vs_pass0={summary['speedup_pass0']:.2f}x, "
        f"speedup_vs_control={summary['speedup_control']:.2f}x"
    )
    if summary["verdict"]:
        print(f"Verdict: {summary['verdict']}")

    if not args.no_plot:
        plotted = plot_result(
            result.stdout,
            plot_path,
            f"cmc_probe {args.arch} CPU {args.core}",
            args.plot_metric,
            plot_ylim,
        )
        if plotted:
            print(f"Saved plot to {plot_path}")

    return 0


if __name__ == "__main__":
    sys.exit(main())
