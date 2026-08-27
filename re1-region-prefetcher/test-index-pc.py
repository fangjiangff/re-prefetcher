#!/usr/bin/env python3
import argparse
import os
import re
import signal
import subprocess
import sys
from pathlib import Path

os.environ.setdefault("MPLCONFIGDIR", "/tmp/matplotlib-cache")
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

plt.rcParams.update({
    "figure.dpi": 150,
    "savefig.dpi": 300,
    "font.family": "DejaVu Sans",
    "font.size": 14,
    "axes.labelsize": 14,
    "axes.titlesize": 16,
    "legend.fontsize": 13,
    "xtick.labelsize": 12,
    "ytick.labelsize": 12,
    "axes.linewidth": 0.8,
    "pdf.fonttype": 42,
    "ps.fonttype": 42,
})

HERE = Path(__file__).resolve().parent
REPO = HERE.parent
STORE_STRIDE_DIR = REPO / "re0-stride-prefetcher"
if str(STORE_STRIDE_DIR) not in sys.path:
    sys.path.insert(0, str(STORE_STRIDE_DIR))

from cross_test_config import ARCH_CONFIG, arch_choices


SRC = HERE / "test-index-pc.c"
UTIL_SRC = STORE_STRIDE_DIR / "until.c"
UTIL_INC = STORE_STRIDE_DIR
BIN_DIR = REPO / "bin"
BIN = BIN_DIR / "region-test-index-pc-pmu"
RESULT_DIR = HERE / "res"
FINAL_RESULT_DIR = HERE / "final-res"
PMU_MODULE_NAME = "prefetch_pmu_event"
PMU_MODULE_DIR = REPO / "kernel-modules" / "prefetch-pmu-event"
DEFAULT_FOOTPRINT_LINES = [4, 1, 19]
DEFAULT_TRIGGER_LINES = [28]
DEFAULT_BASE_PC = "0x500000120"
DEFAULT_MIN_DIFF_BIT = 3
DEFAULT_MAX_DIFF_BIT = 47

BIT_MARKER_RE = re.compile(r"^# ---- diff_bit=(\d+) ----$")
PMU_RE = re.compile(r"^# PMU\s+(\S+)\s+per_round=([0-9.]+)")


def format_line_list(lines):
    return ",".join(str(line) for line in lines)


def parse_line_list(value):
    return [int(item.strip()) for item in value.split(",") if item.strip()]


def parse_args():
    parser = argparse.ArgumentParser(
        description=(
            "PMU-only region-prefetcher PC-index bit-flip experiment. "
            "Plots L2 prefetch events/round against the flipped PC bit."
        )
    )
    parser.add_argument(
        "--arch",
        required=True,
        choices=arch_choices(),
        help="Target architecture; selects the default CPU and PMU events.",
    )
    parser.add_argument("--cc", default="gcc", help="C compiler. Default: gcc")
    parser.add_argument(
        "--core", "--cpu", dest="core", type=int, default=None,
        help="Override the CPU used by taskset.",
    )
    parser.add_argument(
        "--type", choices=("load", "store"), default="load",
        help="Instruction used for region training and triggers.",
    )
    parser.add_argument("--rounds", type=int, default=4000)
    parser.add_argument("--base-pc", default=DEFAULT_BASE_PC)
    parser.add_argument("--min-diff-bit", type=int, default=DEFAULT_MIN_DIFF_BIT)
    parser.add_argument("--max-diff-bit", type=int, default=DEFAULT_MAX_DIFF_BIT)
    parser.add_argument(
        "--train-regions", type=int, default=None,
        help="Default: X925=26, A725=1.",
    )
    parser.add_argument("--wait-nops", type=int, default=1000)
    parser.add_argument("--pmu-device", default=None)
    parser.add_argument(
        "--footprint-lines",
        default=format_line_list(DEFAULT_FOOTPRINT_LINES),
    )
    parser.add_argument(
        "--trigger-lines",
        default=format_line_list(DEFAULT_TRIGGER_LINES),
    )
    parser.add_argument(
        "--output",
        help=(
            "PMU text output. Default: res/test-index-pc-ARCH-pmu.txt, or "
            "final-res/test-index-pc-ARCH-pmu.txt with --final."
        ),
    )
    parser.add_argument(
        "--final",
        action="store_true",
        help=(
            "Write default PMU results and plots to region-prefetcher/final-res "
            "instead of the git-ignored region-prefetcher/res directory."
        ),
    )
    parser.add_argument(
        "--plot", action=argparse.BooleanOptionalAction, default=True,
        help="Generate the L2 prefetch line plot.",
    )
    parser.add_argument(
        "--plot-output",
        help="PNG output. Default: res/test-index-pc-ARCH-l2-by-bit.png.",
    )
    parser.add_argument(
        "--no-run", action="store_true",
        help="Build the PMU-only binary without running it.",
    )
    args = parser.parse_args()

    if args.arch == "Zen4":
        parser.error("test-index-pc.c supports AArch64 only")
    if args.arch not in {"X925", "A725"}:
        parser.error("direct PMU counters support only X925 and A725")
    if args.core is None:
        args.core = ARCH_CONFIG[args.arch]["core"]
    if args.train_regions is None:
        args.train_regions = 26 if args.arch == "X925" else 1
    if args.core < 0 or args.rounds < 1 or args.train_regions < 1:
        parser.error("core must be non-negative; rounds/train-regions must be positive")
    if args.wait_nops < 0:
        parser.error("--wait-nops must be non-negative")

    try:
        base_pc = int(args.base_pc, 0)
    except ValueError:
        parser.error("--base-pc must be an integer address")
    if base_pc & 3:
        parser.error("--base-pc must be 4-byte aligned")
    if args.min_diff_bit < 3:
        parser.error("--min-diff-bit must be >= 3")
    if args.max_diff_bit < args.min_diff_bit or args.max_diff_bit > 47:
        parser.error("--max-diff-bit must be in [min-diff-bit, 47]")

    try:
        args.footprint_lines_parsed = parse_line_list(args.footprint_lines)
        args.trigger_lines_parsed = parse_line_list(args.trigger_lines)
    except ValueError:
        parser.error("footprint/trigger lines must be comma-separated integers")
    if not args.footprint_lines_parsed or not args.trigger_lines_parsed:
        parser.error("footprint/trigger line lists must not be empty")
    bad_lines = [
        line
        for line in args.footprint_lines_parsed + args.trigger_lines_parsed
        if line < 0 or line >= 64
    ]
    if bad_lines:
        parser.error("footprint/trigger lines must be in [0, 63]")
    return args


def c_initializer(lines):
    return "{" + ",".join(str(line) for line in lines) + "}"


def build(args):
    BIN_DIR.mkdir(parents=True, exist_ok=True)
    command = [
        args.cc,
        "-O2",
        "-Wall",
        "-Wextra",
        "-march=armv8.5-a+predres",
        f"-DROUNDS={args.rounds}",
        f"-DTRAIN_REGIONS={args.train_regions}",
        f"-DPREFETCH_WAIT_NOPS={args.wait_nops}",
        "-DPREFETCH_WAIT_NS=0",
        "-DENABLE_PMU=1",
        "-DENABLE_CACHE_PROBE=0",
        f"-DACCESS_TYPE_STORE={int(args.type == 'store')}",
        f"-DPMU_CORE_X925={int(args.arch == 'X925')}",
        "-DPMU_CORE_A55=0",
        f"-DFOOTPRINT_LINES={c_initializer(args.footprint_lines_parsed)}",
        f"-DTRIGGER_LINES={c_initializer(args.trigger_lines_parsed)}",
        f"-I{UTIL_INC}",
        str(SRC),
        str(UTIL_SRC),
        "-o",
        str(BIN),
    ]
    print("# build PMU-only:", " ".join(command))
    subprocess.run(command, check=True)


def run_binary(args, diff_bit):
    command = [
        "taskset", "-c", str(args.core), str(BIN),
        args.base_pc, str(diff_bit),
    ]
    environment = os.environ.copy()
    if args.pmu_device:
        environment["PMU_DEVICE"] = args.pmu_device
    print("# run PMU-only:", " ".join(command))
    return subprocess.run(
        command,
        check=False,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        env=environment,
    )


def failure_status(completed):
    if completed.returncode == 0:
        return None
    if completed.returncode < 0:
        try:
            return signal.Signals(-completed.returncode).name
        except ValueError:
            return f"signal {-completed.returncode}"
    return f"status {completed.returncode}"


def run_module_script(script, *arguments):
    command = ["sudo", str(script), *(str(value) for value in arguments)]
    print("# module:", " ".join(command))
    completed = subprocess.run(command, check=False)
    if completed.returncode != 0:
        raise RuntimeError(
            f"{' '.join(command)} exited with {completed.returncode}"
        )


def unload_direct_pmu_module():
    run_module_script(PMU_MODULE_DIR / "module_unload.sh")


def load_direct_pmu_module(args):
    module_dir = Path("/sys/module") / PMU_MODULE_NAME
    if module_dir.is_dir():
        target_path = module_dir / "parameters" / "target_cpu"
        try:
            target = target_path.read_text().strip()
        except OSError:
            target = "unknown"
        raise RuntimeError(
            f"{PMU_MODULE_NAME} is already loaded for CPU{target}; "
            "refusing to replace it"
        )

    run_module_script(PMU_MODULE_DIR / "module_load.sh", "--cpu", args.core)
    try:
        target = int(
            (module_dir / "parameters" / "target_cpu").read_text().strip(), 0
        )
        if target != args.core:
            raise RuntimeError(
                f"PMU module targets CPU{target}, expected CPU{args.core}"
            )
        perf_access = Path("/proc/sys/kernel/perf_user_access")
        if perf_access.is_file() and perf_access.read_text().strip() != "1":
            raise RuntimeError("kernel.perf_user_access must be 1")
    except Exception:
        unload_direct_pmu_module()
        raise
    print(f"# direct PMU module targets {args.arch} CPU{args.core}")


def parse_results(text):
    rows = []
    current_bit = None
    current_section = None
    sections = {}
    unmapped = set()

    def finish_bit():
        if current_bit is None:
            return
        same = sections.get("same_pc", {})
        flipped = sections.get(f"bit_{current_bit}", {})
        rows.append({
            "bit": current_bit,
            "mapped": current_bit not in unmapped,
            "same_l1": same.get("l1d_cache_hwprf"),
            "same_l2": same.get("l2d_cache_hwprf"),
            "flipped_l1": flipped.get("l1d_cache_hwprf"),
            "flipped_l2": flipped.get("l2d_cache_hwprf"),
        })

    for line in text.splitlines():
        marker = BIT_MARKER_RE.match(line)
        if marker:
            finish_bit()
            current_bit = int(marker.group(1))
            current_section = None
            sections = {}
            continue
        if current_bit is None:
            continue
        if line.startswith("# PC_INDEX_UNMAPPED"):
            unmapped.add(current_bit)
            continue
        if line.startswith("[") and line.endswith("]"):
            current_section = line[1:-1]
            sections.setdefault(current_section, {})
            continue
        match = PMU_RE.match(line)
        if match and current_section is not None:
            sections[current_section][match.group(1)] = float(match.group(2))
    finish_bit()
    return rows


def format_table(rows):
    lines = [
        "# ---- PMU bit summary ----",
        "diff_bit\tmapped\tsame_pc_l1_per_round\tsame_pc_l2_per_round\t"
        "flipped_pc_l1_per_round\tflipped_pc_l2_per_round\tl2_delta",
    ]
    for row in rows:
        values = [
            str(row["bit"]),
            "yes" if row["mapped"] else "no",
        ]
        for key in ("same_l1", "same_l2", "flipped_l1", "flipped_l2"):
            value = row[key]
            values.append("-" if value is None else f"{value:.4f}")
        if row["same_l2"] is None or row["flipped_l2"] is None:
            values.append("-")
        else:
            values.append(f"{row['flipped_l2'] - row['same_l2']:+.4f}")
        lines.append("\t".join(values))
    return "\n".join(lines) + "\n"


def output_path(args):
    if args.output:
        path = Path(args.output)
    else:
        result_dir = FINAL_RESULT_DIR if args.final else RESULT_DIR
        path = result_dir / f"test-index-pc-{args.arch}-pmu.txt"
    path.parent.mkdir(parents=True, exist_ok=True)
    return path


def plot_path(args):
    if args.plot_output:
        path = Path(args.plot_output)
    else:
        result_dir = FINAL_RESULT_DIR if args.final else RESULT_DIR
        path = result_dir / f"test-index-pc-{args.arch}-l2-by-bit.png"
    path.parent.mkdir(parents=True, exist_ok=True)
    return path


def plot_l2_by_bit(args, rows, path):
    if not any(
        row["flipped_l2"] is not None and row["same_l2"] is not None
        for row in rows
    ):
        print("# no complete L2 PMU rows; plot skipped", file=sys.stderr)
        return False

    bits = [row["bit"] for row in rows]
    flipped = [
        float("nan") if row["flipped_l2"] is None else row["flipped_l2"]
        for row in rows
    ]
    baseline = [
        float("nan") if row["same_l2"] is None else row["same_l2"]
        for row in rows
    ]

    fig, axis = plt.subplots(figsize=(10.0, 4.8), constrained_layout=True)
    axis.plot(
        bits, flipped,
        color="#0072B2", marker="o", markersize=4.0, linewidth=1.6,
        label="Bit-flipped trigger PC",
    )
    axis.plot(
        bits, baseline,
        color="#D55E00", linewidth=1.2, linestyle="--", alpha=0.8,
        label="Same-PC baseline",
    )
    axis.fill_between(bits, 0, flipped, color="#0072B2", alpha=0.08)
    axis.set_xlabel("Flipped PC bit position")
    axis.set_ylabel("L2 prefetch events / round")
    axis.set_title(
        f"{args.arch} region prefetcher: L2 PMU response to PC-bit flips",
        loc="left",
    )
    axis.set_xticks(bits)
    axis.tick_params(axis="x", labelrotation=90)
    axis.set_xlim(min(bits) - 0.5, max(bits) + 0.5)
    axis.set_ylim(bottom=0)
    axis.grid(axis="both", color="#D9D9D9", linewidth=0.7)
    axis.set_axisbelow(True)
    axis.spines["top"].set_visible(False)
    axis.spines["right"].set_visible(False)
    axis.legend(frameon=False)
    fig.savefig(path)
    plt.close(fig)
    return True


def main():
    args = parse_args()
    build(args)
    if args.no_run:
        print(f"# PMU-only binary: {BIN}")
        return 0

    try:
        load_direct_pmu_module(args)
    except RuntimeError as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 2

    try:
        chunks = []
        for diff_bit in range(args.min_diff_bit, args.max_diff_bit + 1):
            completed = run_binary(args, diff_bit)
            status = failure_status(completed)
            if status is not None:
                print(
                    f"error: PMU-only bit {diff_bit} failed: {status}",
                    file=sys.stderr,
                )
                if completed.stdout:
                    print(completed.stdout, file=sys.stderr, end="")
                return 128 - completed.returncode if completed.returncode < 0 else completed.returncode
            print(f"# ---- PMU diff_bit={diff_bit} ----")
            print(
                completed.stdout,
                end="" if completed.stdout.endswith("\n") else "\n",
            )
            chunks.append(
                f"# ---- diff_bit={diff_bit} ----\n{completed.stdout}"
            )

        raw_text = "\n".join(chunks)
        rows = parse_results(raw_text)
        expected = args.max_diff_bit - args.min_diff_bit + 1
        if len(rows) != expected:
            print(
                f"error: parsed {len(rows)} bit rows, expected {expected}",
                file=sys.stderr,
            )
            return 3

        result_path = output_path(args)
        result_path.write_text(raw_text + "\n" + format_table(rows))
        print(f"# output PMU-only: {result_path}")

        if args.plot:
            image_path = plot_path(args)
            if plot_l2_by_bit(args, rows, image_path):
                print(f"# plot L2-by-bit: {image_path}")
        return 0
    finally:
        try:
            unload_direct_pmu_module()
        except RuntimeError as exc:
            print(f"error: automatic module unload failed: {exc}",
                  file=sys.stderr)


if __name__ == "__main__":
    raise SystemExit(main())
