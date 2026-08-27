#!/usr/bin/env python3

import argparse
import csv
import os
import re
import subprocess
import sys

ROOT_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
if ROOT_DIR not in sys.path:
    sys.path.insert(0, ROOT_DIR)

from cross_test_config import ARCH_CONFIG, arch_choices


BASE_DIR = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.join(BASE_DIR, "test0-trigger-fault.c")
UTIL_SRC = os.path.join(ROOT_DIR, "until.c")
OUT = os.path.join(ROOT_DIR, "bin", "test0-trigger-fault")

RESULT_DIR = os.path.join(ROOT_DIR, "res", "store-stride-fault")
RAW_DIR = os.path.join(RESULT_DIR, "raw")
DUMP_DIR = os.path.join(RESULT_DIR, "dump")
PLOT_DIR = os.path.join(ROOT_DIR, "res", "barplots-fault")

DEFAULT_MODES = "baseline,trigger-skip,all-skip"
MODE_CONFIG = {
    "baseline": {"scope": 0, "retry": 0},
    "trigger-skip": {"scope": 1, "retry": 0},
    "all-skip": {"scope": 2, "retry": 0},
    "trigger-retry": {"scope": 1, "retry": 1},
    "all-retry": {"scope": 2, "retry": 1},
}

ROLE_COLORS = {
    "accessed": "#D55E00",
    "faulted": "#CC79A7",
    "prefetched": "#0072B2",
    "cache_miss": "#BDBDBD",
}


def parse_csv(value):
    return [item.strip() for item in value.split(",") if item.strip()]


def parse_args():
    parser = argparse.ArgumentParser(
        description=(
            "Build and run test0-trigger-fault.c control modes for Arm "
            "store-stride prefetcher page-fault experiments."
        )
    )
    parser.add_argument("--arch", choices=arch_choices(), default=None,
                        help="Test one architecture label; default tests all configured labels.")
    parser.add_argument("--core", type=int, default=None,
                        help="Override the taskset core; requires --arch.")
    parser.add_argument("--arches", default=None,
                        help="Comma-separated architecture labels.")
    parser.add_argument("--cores", default=None,
                        help="Comma-separated core overrides for --arches.")
    parser.add_argument("--modes", default=DEFAULT_MODES,
                        help=("Comma-separated modes: baseline, trigger-skip, all-skip, "
                              "trigger-retry, all-retry. Default: " + DEFAULT_MODES))
    parser.add_argument("--stride", type=int, default=5,
                        help="Stride in cache lines. Default: 5")
    parser.add_argument("--train-step", type=int, default=3,
                        help="Number of stride accesses including the trigger. Default: 3")
    parser.add_argument("--rounds", type=int, default=40000,
                        help="Measurement rounds per mode. Default: 40000")
    parser.add_argument("--probe-positions", type=int, default=100,
                        help="Number of cache-line positions in the probe sweep. Default: 100")
    parser.add_argument("--probe-mode", choices=["sweep", "single"], default="sweep",
                        help="Rotate over all positions or probe only one position.")
    parser.add_argument("--single-probe-position", type=int, default=None,
                        help="Position for single mode; default is train_step * stride.")
    parser.add_argument("--threshold", type=int, default=None,
                        help="Cache-hit threshold in the selected timer unit.")
    parser.add_argument("--timer", choices=["gettime", "rdtsc", "cntvct", "pmccntr"],
                        default=None, help="Timestamp source; default depends on architecture.")
    parser.add_argument("--cc", default=os.environ.get("CC", "gcc"),
                        help="Compiler command. Default: $CC or gcc")
    parser.add_argument("--objdump", default=os.environ.get("OBJDUMP", "objdump"),
                        help="Objdump command. Default: $OBJDUMP or objdump")
    parser.add_argument("--no-static", action="store_true",
                        help="Do not pass -static to the compiler.")
    parser.add_argument("--no-dump", action="store_true",
                        help="Do not save disassembly for each mode.")
    parser.add_argument("--no-plot", action="store_true",
                        help="Do not generate per-mode bar charts.")
    parser.add_argument("--plot-only", action="store_true",
                        help="Read existing TSV files without compiling or running.")
    args = parser.parse_args()

    if args.core is not None and args.arch is None:
        parser.error("--core requires --arch")
    if args.cores is not None and args.arches is None:
        parser.error("--cores requires --arches")
    if (args.arch is not None) and (args.arches is not None or args.cores is not None):
        parser.error("Use either --arch/--core or --arches/--cores")
    if args.core is not None and args.core < 0:
        parser.error("--core must be >= 0")
    if args.stride < 1 or args.train_step < 1 or args.rounds < 1:
        parser.error("--stride, --train-step, and --rounds must be >= 1")
    if args.probe_positions < 1:
        parser.error("--probe-positions must be >= 1")
    if args.probe_mode == "sweep" and args.rounds < args.probe_positions:
        parser.error("--rounds must be >= --probe-positions in sweep mode")
    if args.single_probe_position is not None and args.single_probe_position < 0:
        parser.error("--single-probe-position must be >= 0")
    if args.threshold is not None and args.threshold < 1:
        parser.error("--threshold must be >= 1")

    modes = parse_csv(args.modes)
    unknown = [mode for mode in modes if mode not in MODE_CONFIG]
    if not modes:
        parser.error("--modes must not be empty")
    if unknown:
        parser.error("unknown mode(s): " + ", ".join(unknown))
    args.modes = modes

    predicted = args.train_step * args.stride
    single = args.single_probe_position if args.single_probe_position is not None else predicted
    if predicted >= args.probe_positions:
        parser.error("predicted position train_step * stride must be inside probe positions")
    if args.probe_mode == "single" and single >= args.probe_positions:
        parser.error("single probe position must be inside probe positions")
    return args


args = parse_args()


def is_x86(arch):
    return arch in {"x86", "Zen4"}


def targets():
    if args.arch is not None:
        core = args.core if args.core is not None else ARCH_CONFIG[args.arch]["core"]
        return [(args.arch, core)]

    arches = parse_csv(args.arches) if args.arches else list(arch_choices())
    unknown = [arch for arch in arches if arch not in ARCH_CONFIG]
    if unknown:
        raise ValueError("unknown architecture(s): " + ", ".join(unknown))
    if args.cores:
        try:
            cores = [int(value) for value in parse_csv(args.cores)]
        except ValueError as exc:
            raise ValueError("--cores must contain integers") from exc
    else:
        cores = [ARCH_CONFIG[arch]["core"] for arch in arches]
    if len(arches) != len(cores):
        raise ValueError("architecture and core lists must have equal length")
    if any(core < 0 for core in cores):
        raise ValueError("cores must be >= 0")
    return list(zip(arches, cores))


def timer_for(arch):
    if args.timer:
        supported = {"gettime", "rdtsc"} if is_x86(arch) else {
            "gettime", "cntvct", "pmccntr"
        }
        if args.timer not in supported:
            raise ValueError(f"timer {args.timer} is unsupported for {arch}")
        return args.timer
    return "gettime" if is_x86(arch) else "cntvct"


def timer_define(arch):
    return {
        "gettime": "-DGETTIME=1",
        "rdtsc": "-DRDTSC=1",
        "cntvct": "-DCNTVCT=1",
        "pmccntr": "-DPMCCNTR=1",
    }[timer_for(arch)]


def timer_unit(arch):
    return {
        "gettime": "ns",
        "rdtsc": "cycles",
        "cntvct": "ticks",
        "pmccntr": "cycles",
    }[timer_for(arch)]


def threshold_for(arch):
    return args.threshold if args.threshold is not None else ARCH_CONFIG[arch]["threshold_ns"]


def predicted_position():
    return args.train_step * args.stride


def single_probe_position():
    return (args.single_probe_position if args.single_probe_position is not None
            else predicted_position())


def test_name(arch, core, mode):
    probe = (f"single{single_probe_position()}" if args.probe_mode == "single"
             else "sweep")
    return (f"{arch}-core{core}-stride{args.stride}-train{args.train_step}-"
            f"{mode}-{timer_for(arch)}-{probe}")


def paths_for(arch, core, mode):
    name = test_name(arch, core, mode)
    return {
        "raw": os.path.join(RAW_DIR, name + ".txt"),
        "tsv": os.path.join(RESULT_DIR, name + ".tsv"),
        "dump": os.path.join(DUMP_DIR, name + ".dump"),
        "plot": os.path.join(PLOT_DIR, name + ".png"),
    }


def ensure_dirs():
    for path in [os.path.dirname(OUT), RESULT_DIR, RAW_DIR, DUMP_DIR, PLOT_DIR]:
        os.makedirs(path, exist_ok=True)


def compile_test(arch, mode):
    config = MODE_CONFIG[mode]
    command = [args.cc, "-std=gnu11", "-O0"]
    if not args.no_static:
        command.append("-static")
    if not is_x86(arch):
        command.append("-march=armv8.5-a+predres")
    command.extend([
        timer_define(arch),
        f"-DSTRIDE_BYTES={args.stride * 64}",
        f"-DTRAIN_STEP={args.train_step}",
        f"-DROUNDS={args.rounds}",
        f"-DPROBE_POSITIONS={args.probe_positions}",
        f"-DSINGLE_PROBE={1 if args.probe_mode == 'single' else 0}",
        f"-DSINGLE_PROBE_POSITION={single_probe_position()}",
        f"-DFAULT_SCOPE={config['scope']}",
        f"-DFAULT_RECOVERY_RETRY={config['retry']}",
        "-o", OUT, SRC, UTIL_SRC,
    ])
    print("Compile:", " ".join(command))
    return subprocess.run(command).returncode == 0


def write_dump(path):
    if args.no_dump:
        return
    result = subprocess.run([args.objdump, "-d", OUT], capture_output=True, text=True)
    if result.returncode != 0:
        print(f"Warning: objdump failed for {path}", file=sys.stderr)
        return
    with open(path, "w") as output:
        output.write(result.stdout)


def expected_faults(mode):
    scope = MODE_CONFIG[mode]["scope"]
    if scope == 0:
        return 0
    if scope == 1:
        return args.rounds
    return args.rounds * args.train_step


def address_role(position, mode):
    scope = MODE_CONFIG[mode]["scope"]
    for step in range(args.train_step):
        if position != step * args.stride:
            continue
        if scope == 2 or (scope == 1 and step == args.train_step - 1):
            return "faulted"
        return "accessed"
    return None


def parse_output(output, arch, mode):
    fault_match = re.search(r"^# faults=(\d+) expected=(\d+)$", output, re.MULTILINE)
    if not fault_match:
        raise ValueError("program output has no fault-count header")
    actual_faults, reported_expected = map(int, fault_match.groups())
    wanted = expected_faults(mode)
    if actual_faults != wanted or reported_expected != wanted:
        raise ValueError(
            f"fault count mismatch: actual={actual_faults}, "
            f"reported_expected={reported_expected}, runner_expected={wanted}"
        )

    threshold = threshold_for(arch)
    rows = []
    for line in output.splitlines():
        stripped = line.strip()
        if not stripped or stripped.startswith("#"):
            continue
        fields = stripped.split()
        if len(fields) != 4:
            raise ValueError(f"unexpected output row: {line}")
        position, offset_bytes, average, probes = map(int, fields)
        role = address_role(position, mode)
        if role is None:
            role = "prefetched" if probes and average <= threshold else "cache_miss"
        rows.append({
            "position": position,
            "offset_bytes": offset_bytes,
            "role": role,
            "avg_latency": average,
            "probes": probes,
        })
    if not rows:
        raise ValueError("program output has no probe rows")
    return rows, actual_faults


def write_tsv(rows, path):
    with open(path, "w", newline="") as output:
        writer = csv.DictWriter(
            output,
            fieldnames=["position", "offset_bytes", "role", "avg_latency", "probes"],
            delimiter="\t",
        )
        writer.writeheader()
        writer.writerows(rows)


def read_tsv(path):
    rows = []
    with open(path, newline="") as source:
        for row in csv.DictReader(source, delimiter="\t"):
            rows.append({
                "position": int(row["position"]),
                "offset_bytes": int(row["offset_bytes"]),
                "role": row["role"],
                "avg_latency": int(row["avg_latency"]),
                "probes": int(row["probes"]),
            })
    return rows


def predicted_row(rows):
    return next((row for row in rows if row["position"] == predicted_position()), None)


def plot_rows(rows, arch, core, mode, path):
    try:
        import matplotlib.pyplot as plt
        from matplotlib.patches import Patch
    except ModuleNotFoundError:
        print("Skipping plots: matplotlib is not installed.")
        return

    rows = sorted(rows, key=lambda row: row["position"])
    positions = [row["position"] for row in rows]
    values = [row["avg_latency"] for row in rows]
    colors = [ROLE_COLORS[row["role"]] for row in rows]
    width = min(max(args.probe_positions / 5, 10), 18)
    fig, ax = plt.subplots(figsize=(width, 4))
    ax.bar(positions, values, color=colors, width=0.85,
           edgecolor="black", linewidth=0.25)
    ax.axhline(threshold_for(arch), color="black", linestyle="--", linewidth=0.9)
    ax.axvline(predicted_position(), color="#0072B2", linestyle=":", linewidth=1.0)
    ax.set_title(f"{arch} core {core}: {mode}, stride={args.stride}, train={args.train_step}")
    ax.set_xlabel("Probe cache-line index")
    ax.set_ylabel(f"Average latency ({timer_unit(arch)})")
    ax.grid(axis="y", alpha=0.25)
    ax.legend(handles=[
        Patch(facecolor=ROLE_COLORS[role], edgecolor="black", label=role)
        for role in ["accessed", "faulted", "prefetched", "cache_miss"]
    ], frameon=False, ncol=4)
    fig.tight_layout()
    fig.savefig(path, dpi=300)
    plt.close(fig)
    print(f"Saved plot: {path}")


def run_one(arch, core, mode):
    paths = paths_for(arch, core, mode)
    print("=" * 72)
    print(f"arch={arch} core={core} mode={mode} stride={args.stride} "
          f"train_step={args.train_step} rounds={args.rounds} "
          f"timer={timer_for(arch)} threshold={threshold_for(arch)} {timer_unit(arch)}")

    if args.plot_only:
        if not os.path.exists(paths["tsv"]):
            print(f"Missing TSV: {paths['tsv']}", file=sys.stderr)
            return None
        rows = read_tsv(paths["tsv"])
        faults = expected_faults(mode)
    else:
        if not compile_test(arch, mode):
            print("Compile failed", file=sys.stderr)
            return None
        write_dump(paths["dump"])
        result = subprocess.run(
            ["taskset", "-c", str(core), OUT], capture_output=True, text=True
        )
        if result.returncode != 0:
            print(f"Execution failed with status {result.returncode}", file=sys.stderr)
            if result.stdout:
                print(result.stdout)
            if result.stderr:
                print(result.stderr, file=sys.stderr)
            return None
        with open(paths["raw"], "w") as output:
            output.write(result.stdout)
        try:
            rows, faults = parse_output(result.stdout, arch, mode)
        except ValueError as exc:
            print(f"Invalid output: {exc}", file=sys.stderr)
            return None
        write_tsv(rows, paths["tsv"])

    if not args.no_plot:
        plot_rows(rows, arch, core, mode, paths["plot"])
    row = predicted_row(rows)
    if row is None or row["probes"] == 0:
        print("Predicted position was not measured", file=sys.stderr)
        return None
    hit = row["avg_latency"] <= threshold_for(arch)
    print(f"Result: mode={mode} faults={faults} predicted={row['position']} "
          f"avg={row['avg_latency']} {timer_unit(arch)} hit={'yes' if hit else 'no'}")
    return {
        "arch": arch,
        "core": core,
        "mode": mode,
        "faults": faults,
        "predicted_position": row["position"],
        "avg_latency": row["avg_latency"],
        "unit": timer_unit(arch),
        "threshold": threshold_for(arch),
        "prefetched": "yes" if hit else "no",
    }


def write_summary(summary):
    path = os.path.join(RESULT_DIR, "summary.tsv")
    fields = ["arch", "core", "mode", "faults", "predicted_position",
              "avg_latency", "unit", "threshold", "prefetched"]
    with open(path, "w", newline="") as output:
        writer = csv.DictWriter(output, fieldnames=fields, delimiter="\t")
        writer.writeheader()
        writer.writerows(summary)
    print(f"Saved summary: {path}")


def main():
    ensure_dirs()
    try:
        selected_targets = targets()
        for arch, _ in selected_targets:
            timer_for(arch)
    except ValueError as exc:
        print(f"Error: {exc}", file=sys.stderr)
        return 2

    summary = []
    failed = False
    for arch, core in selected_targets:
        for mode in args.modes:
            result = run_one(arch, core, mode)
            if result is None:
                failed = True
            else:
                summary.append(result)
    if summary:
        write_summary(summary)
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
