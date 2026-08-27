#!/usr/bin/env python3

import argparse
import os
import platform
import re
import shutil
import signal
import subprocess
import sys
from pathlib import Path


HERE = Path(__file__).resolve().parent
REPO = HERE.parent
SRC = HERE / "test-cmc.c"
UTIL_SRC = REPO / "re0-stride-prefetcher" / "until.c"
BIN_DIR = HERE / "bin"
RESULT_DIR = HERE / "res"
PMU_MODULE_NAME = "prefetch_pmu_event"
PMU_MODULE_DIR = REPO / "kernel-modules" / "prefetch-pmu-event"

ARCH_CONFIG = {
    "X925": {"core": 6, "threshold_ns": 80},
    "A725": {"core": 4, "threshold_ns": 120},
}

PMU_RE = re.compile(r"^# PMU\s+(\S+)\s+per_round=([0-9.]+)")


def default_compiler():
    if os.environ.get("CC"):
        return os.environ["CC"]
    if platform.machine().lower() in ("aarch64", "arm64"):
        return "gcc"
    if shutil.which("aarch64-linux-gnu-gcc"):
        return "aarch64-linux-gnu-gcc"
    return "gcc"


def parse_args():
    parser = argparse.ArgumentParser(
        description=(
            "Basic CMC existence experiment: compare an untrained trigger "
            "with the same trigger after training node[i] -> node[i+1]."
        )
    )
    parser.add_argument("--arch", choices=ARCH_CONFIG, default="A725")
    parser.add_argument("--core", "--cpu", dest="core", type=int)
    parser.add_argument("--rounds", type=int, default=5000)
    parser.add_argument("--threshold-ns", type=int)
    parser.add_argument("--training-replays", type=int, default=8)
    parser.add_argument("--wait-nops", type=int, default=1000)
    parser.add_argument(
        "--pmu", action=argparse.BooleanOptionalAction, default=True,
        help="Build/run ENABLE_PMU=1, ENABLE_CACHE_PROBE=0.",
    )
    parser.add_argument(
        "--cache-probe", action=argparse.BooleanOptionalAction, default=True,
        help="Build/run ENABLE_PMU=0, ENABLE_CACHE_PROBE=1.",
    )
    parser.add_argument("--cc", "--compiler", dest="cc", default=default_compiler())
    parser.add_argument("--output")
    parser.add_argument("--plot")
    parser.add_argument("--no-plot", action="store_true")
    parser.add_argument("--no-build", action="store_true")
    parser.add_argument("--no-run", "--build-only", dest="no_run",
                        action="store_true")
    args = parser.parse_args()

    config = ARCH_CONFIG[args.arch]
    if args.core is None:
        args.core = config["core"]
    if args.threshold_ns is None:
        args.threshold_ns = config["threshold_ns"]
    if args.rounds <= 0 or args.threshold_ns <= 0 or args.training_replays <= 0:
        parser.error("rounds, threshold-ns, and training-replays must be positive")
    if args.core < 0 or args.wait_nops < 0:
        parser.error("core and wait-nops must be non-negative")
    if not args.pmu and not args.cache_probe:
        parser.error("enable at least one of --pmu and --cache-probe")
    return args


def binary_path(kind):
    return BIN_DIR / f"test-cmc-basic-{kind}"


def build(args, kind, enable_pmu, enable_cache_probe):
    output = binary_path(kind)
    BIN_DIR.mkdir(parents=True, exist_ok=True)
    command = [
        args.cc,
        "-x", "c",
        "-std=gnu11",
        "-O2",
        "-Wall",
        "-Wextra",
        "-march=armv8.5-a+predres",
        "-DENABLE_CPP_RCTX=1",
        f"-DENABLE_PMU={int(enable_pmu)}",
        f"-DENABLE_CACHE_PROBE={int(enable_cache_probe)}",
        f"-DPREFETCH_WAIT_NOPS={args.wait_nops}",
        f"-DPMU_CORE_X925={int(args.arch == 'X925')}",
        str(SRC),
        str(UTIL_SRC),
        "-o", str(output),
    ]
    print(f"# build {kind}:", " ".join(str(item) for item in command))
    subprocess.run(command, check=True)
    return output


def run_arguments(args):
    return [str(args.rounds), str(args.threshold_ns),
            str(args.training_replays)]


def run_binary(args, binary, kind):
    if platform.machine().lower() not in ("aarch64", "arm64"):
        raise RuntimeError("execution requires an AArch64 host; use --no-run")
    command = [
        "taskset", "-c", str(args.core), str(binary), *run_arguments(args)
    ]
    print(f"# run {kind}:", " ".join(command))
    completed = subprocess.run(
        command, check=False, text=True,
        stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
    )
    return completed.returncode, completed.stdout


def report_failure(kind, returncode, output):
    if returncode == 0:
        return False
    if returncode < 0:
        try:
            reason = signal.Signals(-returncode).name
        except ValueError:
            reason = f"signal {-returncode}"
    else:
        reason = f"status {returncode}"
    print(f"error: {kind} run failed with {reason}", file=sys.stderr)
    if output:
        print(output, end="" if output.endswith("\n") else "\n", file=sys.stderr)
    return True


def module_script(name, *arguments):
    command = ["sudo", str(PMU_MODULE_DIR / name),
               *(str(value) for value in arguments)]
    print("# module:", " ".join(command))
    completed = subprocess.run(command, check=False)
    if completed.returncode != 0:
        raise RuntimeError(
            f"{' '.join(command)} exited with {completed.returncode}"
        )


def load_pmu_module(args):
    module_dir = Path("/sys/module") / PMU_MODULE_NAME
    if module_dir.is_dir():
        raise RuntimeError(
            f"{PMU_MODULE_NAME} is already loaded; refusing to replace it"
        )
    module_script("module_load.sh", "--cpu", args.core)
    try:
        target_path = module_dir / "parameters" / "target_cpu"
        target_cpu = int(target_path.read_text().strip(), 0)
        if target_cpu != args.core:
            raise RuntimeError(
                f"PMU module targets CPU{target_cpu}, expected CPU{args.core}"
            )
        perf_access = Path("/proc/sys/kernel/perf_user_access")
        if perf_access.is_file() and perf_access.read_text().strip() != "1":
            raise RuntimeError("kernel.perf_user_access must be 1")
    except Exception:
        module_script("module_unload.sh")
        raise
    print(f"# direct PMU module targets CPU{args.core}")


def unload_pmu_module():
    module_script("module_unload.sh")


def parse_output(text):
    section = None
    pmu = {}
    probes = {}
    for raw_line in text.splitlines():
        line = raw_line.strip()
        if line.startswith("[") and line.endswith("]"):
            section = line[1:-1]
            pmu.setdefault(section, {})
            probes.setdefault(section, [])
            continue
        if section is None:
            continue
        match = PMU_RE.match(line)
        if match:
            pmu[section][match.group(1)] = float(match.group(2))
            continue
        if not line or line.startswith("#"):
            continue
        fields = line.split()
        if len(fields) != 8:
            continue
        try:
            probes[section].append({
                "node": int(fields[0]),
                "trigger_page": int(fields[1]),
                "target_page": int(fields[2]),
                "target_line": int(fields[3]),
                "probes": int(fields[4]),
                "hits": int(fields[5]),
                "per_1000": int(fields[6]),
                "avg_latency_ns": int(fields[7]),
            })
        except ValueError:
            continue
    return pmu, probes


def parse_pmu_nodes(text):
    section = None
    rows = {}
    in_node_table = False
    for raw_line in text.splitlines():
        line = raw_line.strip()
        if line.startswith("[") and line.endswith("]"):
            section = line[1:-1]
            rows.setdefault(section, [])
            in_node_table = False
            continue
        if line.startswith("# node"):
            in_node_table = "pmu_samples" in line
            continue
        if section is None or not in_node_table or not line or line.startswith("#"):
            continue
        fields = line.split()
        if len(fields) != 6:
            continue
        try:
            rows[section].append({
                "node": int(fields[0]),
                "pmu_samples": int(fields[1]),
                "l1_events": int(fields[2]),
                "avg_l1_pmu": float(fields[3]),
                "l2_events": int(fields[4]),
                "avg_l2_pmu": float(fields[5]),
            })
        except ValueError:
            continue
    return rows


def merged_table(pmu_text, cache_text):
    _, cache_cases = parse_output(cache_text)
    pmu_cases = parse_pmu_nodes(pmu_text)
    case_order = ["untrained", "trained_A_to_B"]
    remaining = (set(cache_cases) | set(pmu_cases)) - set(case_order)
    case_order.extend(sorted(remaining))
    header = (
        "case\tnode\ttrigger_page\ttarget_page\ttarget_line\t"
        "probes\thits\tcache_per_1000\tavg_latency_ns\t"
        "pmu_samples\tl1_events\tavg_l1_pmu\tl2_events\tavg_l2_pmu"
    )
    lines = ["# Per-node cache-probe and PMU results from separate runs", header]
    for case in case_order:
        cache_by_node = {
            row["node"]: row for row in cache_cases.get(case, [])
        }
        pmu_by_node = {
            row["node"]: row for row in pmu_cases.get(case, [])
        }
        for node in sorted(set(cache_by_node) | set(pmu_by_node)):
            cache = cache_by_node.get(node, {})
            pmu = pmu_by_node.get(node, {})
            values = [
                case, node,
                cache.get("trigger_page", "-"),
                cache.get("target_page", "-"),
                cache.get("target_line", "-"),
                cache.get("probes", "-"),
                cache.get("hits", "-"),
                cache.get("per_1000", "-"),
                cache.get("avg_latency_ns", "-"),
                pmu.get("pmu_samples", "-"),
                pmu.get("l1_events", "-"),
                (f'{pmu["avg_l1_pmu"]:.4f}'
                 if "avg_l1_pmu" in pmu else "-"),
                pmu.get("l2_events", "-"),
                (f'{pmu["avg_l2_pmu"]:.4f}'
                 if "avg_l2_pmu" in pmu else "-"),
            ]
            lines.append("\t".join(str(value) for value in values))
    return "\n".join(lines) + "\n"


def print_green(message):
    if sys.stdout.isatty():
        print(f"\033[32m{message}\033[0m")
    else:
        print(message)


def print_summary(pmu_text, cache_text):
    pmu, _ = parse_output(pmu_text)
    _, probes = parse_output(cache_text)

    untrained_pmu = pmu.get("untrained", {})
    trained_pmu = pmu.get("trained_A_to_B", {})
    for event in sorted(set(untrained_pmu) | set(trained_pmu)):
        before = untrained_pmu.get(event, 0.0)
        after = trained_pmu.get(event, 0.0)
        print_green(
            f"# delta {event}={after - before:.4f}/round "
            f"(trained={after:.4f}, untrained={before:.4f})"
        )

    rates = {}
    for case, rows in probes.items():
        total_probes = sum(row["probes"] for row in rows)
        total_hits = sum(row["hits"] for row in rows)
        rates[case] = 0.0 if total_probes == 0 else total_hits * 1000 / total_probes
    if "trained_A_to_B" in rates and "untrained" in rates:
        print_green(
            f"# delta cache_hits_per_1000="
            f"{rates['trained_A_to_B'] - rates['untrained']:.2f} "
            f"(trained={rates['trained_A_to_B']:.2f}, "
            f"untrained={rates['untrained']:.2f})"
        )


def output_paths(args):
    if args.output:
        combined = Path(args.output)
    else:
        RESULT_DIR.mkdir(parents=True, exist_ok=True)
        combined = RESULT_DIR / (
            f"cmc-basic-{args.arch}-cpu{args.core}-thr{args.threshold_ns}.txt"
        )
    combined.parent.mkdir(parents=True, exist_ok=True)
    return (
        combined,
        combined.with_name(combined.stem + "-pmu" + combined.suffix),
        combined.with_name(combined.stem + "-cache" + combined.suffix),
    )


def save_outputs(args, outputs):
    combined, pmu_path, cache_path = output_paths(args)
    if "pmu" in outputs:
        pmu_path.write_text(outputs["pmu"])
        print(f"# output PMU: {pmu_path}")
    if "cache" in outputs:
        cache_path.write_text(outputs["cache"])
        print(f"# output cache: {cache_path}")
    combined.write_text(merged_table(
        outputs.get("pmu", ""), outputs.get("cache", "")
    ))
    print(f"# output merged table: {combined}")


def plot_pmu(args, text):
    if args.no_plot:
        return
    cases = parse_pmu_nodes(text)
    cases = {name: rows for name, rows in cases.items() if rows}
    if not cases:
        return
    try:
        os.environ.setdefault("MPLCONFIGDIR", "/tmp/matplotlib-cache")
        import matplotlib
        matplotlib.use("Agg")
        import matplotlib.pyplot as plt
    except ImportError:
        print("# matplotlib unavailable; skip plot", file=sys.stderr)
        return

    figure, (l1_axis, l2_axis) = plt.subplots(
        2, 1, figsize=(8, 6.0), sharex=True, sharey=True,
        constrained_layout=True
    )
    for name, rows in cases.items():
        rows = sorted(rows, key=lambda row: row["node"])
        nodes = [row["node"] for row in rows]
        l1_values = [row["avg_l1_pmu"] for row in rows]
        l2_values = [row["avg_l2_pmu"] for row in rows]
        line_style = {
            "marker": "o",
            "markersize": 2.5,
            "linewidth": 1.0,
            "label": name,
        }
        l1_axis.plot(nodes, l1_values, **line_style)
        l2_axis.plot(nodes, l2_values, **line_style)
        for axis, values in ((l1_axis, l1_values),
                             (l2_axis, l2_values)):
            for node, value in zip(nodes, values):
                if value != 0.0:
                    axis.annotate(
                        str(node), (node, value),
                        xytext=(0, 4), textcoords="offset points",
                        ha="center", va="bottom", fontsize=6,
                    )

    l1_axis.set_ylabel("Average L1 PMU events")
    l1_axis.set_title(f"PMU events by node: {args.arch} CPU{args.core}")
    l2_axis.set_xlabel("Node")
    l2_axis.set_ylabel("Average L2 PMU events")
    for axis in (l1_axis, l2_axis):
        axis.grid(axis="y", alpha=0.25)
        axis.legend()
    path = Path(args.plot) if args.plot else output_paths(args)[0].with_suffix(".png")
    path.parent.mkdir(parents=True, exist_ok=True)
    figure.savefig(path, dpi=300)
    plt.close(figure)
    print(f"# plot: {path}")


def main():
    args = parse_args()
    binaries = {}
    if args.pmu:
        binaries["pmu"] = binary_path("pmu") if args.no_build else build(
            args, "pmu", enable_pmu=True, enable_cache_probe=False
        )
    if args.cache_probe:
        binaries["cache"] = binary_path("cache") if args.no_build else build(
            args, "cache", enable_pmu=False, enable_cache_probe=True
        )

    if args.no_run:
        for kind, path in binaries.items():
            print(f"# binary {kind}: {path}")
        return 0
    for path in binaries.values():
        if not path.exists():
            raise SystemExit(f"binary not found: {path}")

    outputs = {}
    pmu_loaded = False
    try:
        if args.pmu:
            load_pmu_module(args)
            pmu_loaded = True
            code, text = run_binary(args, binaries["pmu"], "PMU-only")
            if report_failure("PMU-only", code, text):
                return 128 - code if code < 0 else code
            outputs["pmu"] = text
            print(text, end="" if text.endswith("\n") else "\n")

        if args.cache_probe:
            code, text = run_binary(args, binaries["cache"], "cache-only")
            if report_failure("cache-only", code, text):
                return 128 - code if code < 0 else code
            outputs["cache"] = text
            print(text, end="" if text.endswith("\n") else "\n")

        save_outputs(args, outputs)
        print_summary(outputs.get("pmu", ""), outputs.get("cache", ""))
        if "pmu" in outputs:
            plot_pmu(args, outputs["pmu"])
        return 0
    finally:
        if pmu_loaded:
            try:
                unload_pmu_module()
            except RuntimeError as exc:
                print(f"error: PMU module unload failed: {exc}", file=sys.stderr)


if __name__ == "__main__":
    raise SystemExit(main())
