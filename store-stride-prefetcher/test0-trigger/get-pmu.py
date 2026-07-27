#!/usr/bin/env python3
"""Sweep test0-exist.c train_step values and plot hardware-prefetch PMU events."""

from __future__ import annotations

import argparse
import csv
import os
from pathlib import Path
import re
import subprocess
import sys


BASE_DIR = Path(__file__).resolve().parent
ROOT_DIR = BASE_DIR.parent
if str(ROOT_DIR) not in sys.path:
    sys.path.insert(0, str(ROOT_DIR))

from cross_test_config import ARCH_CONFIG, arch_choices


os.environ.setdefault("MPLCONFIGDIR", "/tmp/matplotlib-cache")

SOURCE = BASE_DIR / "test0-exist.c"
UTIL_SOURCE = ROOT_DIR / "until.c"
BINARY = ROOT_DIR / "bin" / "test0-exist-pmu"

DEFAULT_MIN_STEP = 0
DEFAULT_MAX_STEP = 40
DEFAULT_STRIDE_LINES = 5
DEFAULT_ROUNDS = 400
DEFAULT_ACCESSES = ("load", "store")
L1D_HWPRF_EVENT = "l1d_cache_hwprf"
L1D_HWPRF_DELTA = "l1d_cache_hwprf_delta"

PMU_VALUE_RE = re.compile(
    r"^# PMU\s+(?P<name>\S+)\s+per_round="
    r"(?P<value>[0-9]+(?:\.[0-9]+)?)$"
)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--arch",
        required=True,
        choices=arch_choices(),
        help="Target architecture; also selects the default CPU core and PMU events.",
    )
    parser.add_argument(
        "--core",
        type=int,
        default=None,
        help="CPU core for taskset. Default is selected from --arch.",
    )
    parser.add_argument(
        "--min-step",
        type=int,
        default=DEFAULT_MIN_STEP,
        help=f"First train_step value. Default: {DEFAULT_MIN_STEP}",
    )
    parser.add_argument(
        "--max-step",
        type=int,
        default=DEFAULT_MAX_STEP,
        help=f"Last train_step value. Default: {DEFAULT_MAX_STEP}",
    )
    parser.add_argument(
        "--accesses",
        nargs="+",
        choices=("load", "store", "prefetch"),
        default=list(DEFAULT_ACCESSES),
        help="Training access types. Default: load store",
    )
    parser.add_argument(
        "--stride",
        type=int,
        default=DEFAULT_STRIDE_LINES,
        help=f"Stride in cache lines. Default: {DEFAULT_STRIDE_LINES}",
    )
    parser.add_argument(
        "--rounds",
        type=int,
        default=DEFAULT_ROUNDS,
        help=f"Measurement rounds per point. Default: {DEFAULT_ROUNDS}",
    )
    parser.add_argument(
        "--cc",
        default=os.environ.get("CC", "gcc"),
        help="C compiler. Default: $CC or gcc",
    )
    parser.add_argument(
        "--pmu-device",
        default=None,
        help="Override the PMU device through the PMU_DEVICE environment variable.",
    )
    parser.add_argument(
        "-o",
        "--output",
        type=Path,
        default=None,
        help="TSV result path. Default: test0-trigger/pmu-<arch>.txt",
    )
    parser.add_argument(
        "--plot-output",
        type=Path,
        default=None,
        help="Plot path. Default: test0-trigger/pmu-<arch>-line.png",
    )
    parser.add_argument(
        "--raw-dir",
        type=Path,
        default=None,
        help="Save each program's stdout/stderr under this directory.",
    )
    parser.add_argument(
        "--plot-only",
        action="store_true",
        help="Read --output and only generate the plot.",
    )
    parser.add_argument("--no-plot", action="store_true", help="Do not generate a plot.")
    args = parser.parse_args()

    if args.arch == "Zen4":
        parser.error("test0-exist.c PMU collection currently supports Arm architectures only")
    if args.core is None:
        args.core = ARCH_CONFIG[args.arch]["core"]
    if args.core < 0:
        parser.error("--core must be >= 0")
    if args.min_step < 0:
        parser.error("--min-step must be >= 0")
    if args.max_step < args.min_step:
        parser.error("--max-step must be >= --min-step")
    if args.stride < 1:
        parser.error("--stride must be >= 1")
    if args.rounds < 1:
        parser.error("--rounds must be >= 1")
    if args.output is None:
        args.output = BASE_DIR / f"pmu-{args.arch.lower()}.txt"
    if args.plot_output is None:
        args.plot_output = BASE_DIR / f"pmu-{args.arch.lower()}-line.png"
    return args


def pmu_defines(arch: str) -> list[str]:
    return [
        f"-DPMU_CORE_X925={int(arch == 'X925')}",
        f"-DPMU_CORE_A55={int(arch == 'A55')}",
    ]


def compile_test(args: argparse.Namespace, train_step: int, access: str) -> None:
    BINARY.parent.mkdir(parents=True, exist_ok=True)
    command = [
        args.cc,
        "-std=gnu11",
        "-O0",
        "-static",
        f"-DSTRIDE_BYTES={args.stride * 64}",
        f"-DTRAIN_STEP={train_step}",
        f"-DROUNDS={args.rounds}",
        "-DENABLE_PROBE=0",
        "-DPROBE_POSITIONS=1",
        "-DSINGLE_PROBE=1",
        "-DSINGLE_PROBE_POSITION=0",
        f"-DENABLE_CPP_RCTX={int(args.arch == 'X925')}",
        "-DENABLE_SCHED_YIELD=0",
        f"-DTRAIN_ACCESS_LOAD={int(access == 'load')}",
        f"-DTRAIN_ACCESS_PREFETCH={int(access == 'prefetch')}",
        *pmu_defines(args.arch),
        "-o",
        str(BINARY),
        str(SOURCE),
        str(UTIL_SOURCE),
    ]
    if args.arch == "X925":
        command.insert(1, "-march=armv8.5-a+predres")

    result = subprocess.run(command, capture_output=True, text=True)
    if result.returncode != 0:
        detail = (result.stderr or result.stdout).strip()
        raise RuntimeError(
            f"compile failed for access={access}, train_step={train_step}"
            + (f":\n{detail}" if detail else "")
        )


def run_test(args: argparse.Namespace) -> subprocess.CompletedProcess[str]:
    environment = os.environ.copy()
    if args.pmu_device:
        environment["PMU_DEVICE"] = args.pmu_device
    return subprocess.run(
        ["taskset", "-c", str(args.core), str(BINARY)],
        capture_output=True,
        text=True,
        env=environment,
    )


def parse_pmu_values(output: str) -> dict[str, float]:
    values: dict[str, float] = {}
    for line in output.splitlines():
        match = PMU_VALUE_RE.match(line.strip())
        if match:
            values[match.group("name")] = float(match.group("value"))
    return values


def save_raw_output(
    raw_dir: Path,
    access: str,
    train_step: int,
    run: subprocess.CompletedProcess[str],
) -> None:
    raw_dir.mkdir(parents=True, exist_ok=True)
    path = raw_dir / f"{access}-train{train_step}.txt"
    text = run.stdout
    if run.stderr:
        text += "\n# stderr\n" + run.stderr
    path.write_text(text, encoding="utf-8")


def collect(args: argparse.Namespace) -> tuple[list[dict[str, object]], list[str]]:
    rows: list[dict[str, object]] = []
    event_names: list[str] = []
    total = len(args.accesses) * (args.max_step - args.min_step + 1)
    completed = 0

    for access in args.accesses:
        for train_step in range(args.min_step, args.max_step + 1):
            completed += 1
            print(
                f"[{completed:>2}/{total}] access={access:<8} "
                f"train_step={train_step:>2}",
                flush=True,
            )
            compile_test(args, train_step, access)
            run = run_test(args)
            if args.raw_dir is not None:
                save_raw_output(args.raw_dir, access, train_step, run)
            if run.returncode != 0:
                detail = (run.stderr or run.stdout).strip()
                raise RuntimeError(
                    f"execution failed for access={access}, train_step={train_step}"
                    + (f":\n{detail}" if detail else "")
                )

            values = parse_pmu_values(run.stdout)
            if not values:
                pmu_notes = [
                    line.strip()
                    for line in (run.stdout + "\n" + run.stderr).splitlines()
                    if "PMU" in line
                ]
                detail = "\n".join(pmu_notes)
                raise RuntimeError(
                    f"no PMU values for access={access}, train_step={train_step}"
                    + (f":\n{detail}" if detail else "")
                )
            for name in values:
                if name not in event_names:
                    event_names.append(name)
            rows.append(
                {
                    "train_step": train_step,
                    "access": access,
                    **values,
                    "status": run.returncode,
                }
            )
    return rows, event_names


def add_l1d_hwprf_delta(
    rows: list[dict[str, object]], event_names: list[str]
) -> list[str]:
    """Add the per-access change from the preceding train_step."""
    if L1D_HWPRF_EVENT not in event_names:
        return event_names

    for access in {str(row["access"]) for row in rows}:
        previous_value = 0.0
        access_rows = sorted(
            (row for row in rows if row["access"] == access),
            key=lambda row: int(row["train_step"]),
        )
        for row in access_rows:
            current_value = float(row[L1D_HWPRF_EVENT])
            row[L1D_HWPRF_DELTA] = round(current_value - previous_value, 10)
            previous_value = current_value

    if L1D_HWPRF_DELTA not in event_names:
        return [*event_names, L1D_HWPRF_DELTA]
    return event_names

def write_results(
    path: Path, rows: list[dict[str, object]], event_names: list[str]
) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", newline="", encoding="utf-8") as output_file:
        writer = csv.DictWriter(
            output_file,
            fieldnames=["train_step", "access", *event_names, "status"],
            delimiter="\t",
        )
        writer.writeheader()
        writer.writerows(rows)
    print(f"Saved {path}")


def read_results(
    path: Path,
) -> tuple[list[dict[str, object]], list[str]]:
    if not path.is_file():
        raise FileNotFoundError(f"result file does not exist: {path}")
    rows: list[dict[str, object]] = []
    with path.open(newline="", encoding="utf-8") as input_file:
        reader = csv.DictReader(input_file, delimiter="\t")
        fieldnames = reader.fieldnames or []
        required = {"train_step", "access"}
        missing = required.difference(fieldnames)
        if missing:
            raise ValueError(f"missing TSV columns: {', '.join(sorted(missing))}")
        event_names = [
            name
            for name in fieldnames
            if name not in {"train_step", "access", "status"}
        ]
        if not event_names:
            raise ValueError("result file contains no PMU event columns")
        for line_number, raw_row in enumerate(reader, start=2):
            try:
                row: dict[str, object] = {
                    "train_step": int(raw_row["train_step"]),
                    "access": raw_row["access"],
                }
                row.update({name: float(raw_row[name]) for name in event_names})
            except (TypeError, ValueError) as error:
                raise ValueError(
                    f"{path}:{line_number}: invalid numeric value"
                ) from error
            rows.append(row)
    if not rows:
        raise ValueError("result file contains no data rows")
    return rows, event_names


def validate_plot_rows(
    rows: list[dict[str, object]], args: argparse.Namespace
) -> list[dict[str, object]]:
    selected = [
        row
        for row in rows
        if args.min_step <= int(row["train_step"]) <= args.max_step
        and str(row["access"]) in args.accesses
    ]
    expected_steps = set(range(args.min_step, args.max_step + 1))
    for access in args.accesses:
        actual_steps = {
            int(row["train_step"])
            for row in selected
            if row["access"] == access
        }
        if actual_steps != expected_steps:
            missing = sorted(expected_steps - actual_steps)
            raise ValueError(
                f"{access} is missing train_step values: "
                + ", ".join(map(str, missing))
            )
    return selected


def plot_results(
    path: Path,
    rows: list[dict[str, object]],
    event_names: list[str],
    args: argparse.Namespace,
) -> None:
    try:
        import matplotlib.pyplot as plt
    except ModuleNotFoundError as error:
        raise RuntimeError(
            "plotting requires matplotlib; install python3-matplotlib"
        ) from error

    rows = validate_plot_rows(rows, args)
    access_colors = {
        "load": ("#7f0000", "#b2182b", "#d6604d", "#f4a582"),
        "store": ("#08306b", "#2171b5", "#6baed6", "#bdd7e7"),
        "prefetch": ("#08306b", "#2171b5", "#6baed6", "#bdd7e7"),
    }
    markers = ("o", "s", "^", "D", "v", "P")
    linestyles = ("-", "--", "-.", ":")

    figure, axis = plt.subplots(figsize=(8.4, 4.5))
    for access in args.accesses:
        access_rows = sorted(
            (row for row in rows if row["access"] == access),
            key=lambda row: int(row["train_step"]),
        )
        colors = access_colors[access]
        for event_index, event_name in enumerate(event_names):
            axis.plot(
                [int(row["train_step"]) for row in access_rows],
                [float(row[event_name]) for row in access_rows],
                color=colors[event_index % len(colors)],
                marker=markers[event_index % len(markers)],
                linestyle=linestyles[event_index % len(linestyles)],
                linewidth=1.6,
                markersize=4.5,
                label=f"{access} - {event_name}",
            )

    axis.set_xlabel("train_step")
    axis.set_ylabel("PMU events per round")
    axis.set_title(
        f"{args.arch} hardware-prefetch PMU events "
        f"(stride={args.stride} cache lines)"
    )
    axis.set_xlim(args.min_step, args.max_step)
    axis.set_xticks(range(args.min_step, args.max_step + 1))
    axis.grid(True, linestyle="--", linewidth=0.5, alpha=0.55)
    axis.legend()
    figure.tight_layout()

    path.parent.mkdir(parents=True, exist_ok=True)
    figure.savefig(path, dpi=300, bbox_inches="tight")
    plt.close(figure)
    print(f"Saved {path}")


def main() -> int:
    args = parse_args()
    try:
        if args.plot_only:
            rows, event_names = read_results(args.output)
        else:
            rows, event_names = collect(args)
        event_names = add_l1d_hwprf_delta(rows, event_names)
        if not args.plot_only:
            write_results(args.output, rows, event_names)
        if not args.no_plot:
            plot_results(args.plot_output, rows, event_names, args)
    except (FileNotFoundError, RuntimeError, ValueError) as error:
        print(f"Error: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
