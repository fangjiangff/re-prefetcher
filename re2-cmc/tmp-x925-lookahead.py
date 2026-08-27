#!/usr/bin/env python3

import argparse
import os
import platform
import shutil
import subprocess
import sys
from pathlib import Path


HERE = Path(__file__).resolve().parent
REPO = HERE.parent
SRC = HERE / "tmp-x925-lookahead.c"
UTIL_SRC = REPO / "re0-stride-prefetcher" / "until.c"
BIN = HERE / "bin" / "tmp-x925-lookahead"
RESULT_DIR = HERE / "res"
PMU_MODULE = "prefetch_pmu_event"
PMU_MODULE_DIR = REPO / "kernel-modules" / "prefetch-pmu-event"


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
            "Temporary X925 CMC attribution test: trigger node[i] and "
            "independently probe temporal successors through node[127]."
        )
    )
    parser.add_argument("--arch", choices=("X925",), default="X925")
    parser.add_argument("--core", "--cpu", dest="core", type=int, default=6)
    parser.add_argument(
        "--samples", type=int, default=40,
        help="Samples for every (node, lookahead, case) tuple.",
    )
    parser.add_argument("--threshold-ns", type=int, default=80)
    parser.add_argument("--training-replays", type=int, default=1)
    parser.add_argument("--max-lookahead", type=int, default=127)
    parser.add_argument(
        "--trigger-node", type=int, default=-1,
        help="Scan only this trigger node; -1 scans every trigger node.",
    )
    parser.add_argument(
        "--prefix-misses", type=int, default=0,
        help="Independent random-page misses between training and trigger.",
    )
    parser.add_argument("--wait-nops", type=int, default=1000)
    parser.add_argument("--cc", default=default_compiler())
    parser.add_argument("--output")
    parser.add_argument("--plot")
    parser.add_argument("--no-plot", action="store_true")
    parser.add_argument("--no-build", action="store_true")
    parser.add_argument("--no-run", "--build-only", dest="no_run",
                        action="store_true")
    args = parser.parse_args()
    if (args.samples <= 0 or args.threshold_ns <= 0 or
            args.training_replays <= 0):
        parser.error("samples, threshold-ns, and training-replays must be positive")
    if args.core < 0 or args.wait_nops < 0:
        parser.error("core and wait-nops must be non-negative")
    if not 1 <= args.max_lookahead <= 127:
        parser.error("max-lookahead must be in [1, 127]")
    if not -1 <= args.trigger_node <= 126:
        parser.error("trigger-node must be -1 or in [0, 126]")
    if not 0 <= args.prefix_misses <= 256:
        parser.error("prefix-misses must be in [0, 256]")
    return args


def build(args):
    BIN.parent.mkdir(parents=True, exist_ok=True)
    command = [
        args.cc, "-x", "c", "-std=gnu11", "-O2", "-Wall", "-Wextra",
        "-march=armv8.5-a+predres",
        "-DENABLE_CPP_RCTX=1",
        "-DENABLE_PMU=1",
        "-DENABLE_CACHE_PROBE=1",
        f"-DPREFETCH_WAIT_NOPS={args.wait_nops}",
        "-DPMU_CORE_X925=1",
        str(SRC), str(UTIL_SRC), "-o", str(BIN),
    ]
    print("# build:", " ".join(command))
    subprocess.run(command, check=True)


def module_script(name, *arguments):
    command = ["sudo", str(PMU_MODULE_DIR / name),
               *(str(value) for value in arguments)]
    print("# module:", " ".join(command))
    subprocess.run(command, check=True)


def load_pmu(args):
    module_dir = Path("/sys/module") / PMU_MODULE
    if module_dir.is_dir():
        raise RuntimeError(f"{PMU_MODULE} is already loaded")
    module_script("module_load.sh", "--cpu", args.core)
    try:
        target = int((module_dir / "parameters" / "target_cpu").read_text())
        if target != args.core:
            raise RuntimeError(
                f"PMU module targets CPU{target}, expected CPU{args.core}"
            )
    except Exception:
        module_script("module_unload.sh")
        raise


def run(args):
    command = [
        "taskset", "-c", str(args.core), str(BIN),
        str(args.samples), str(args.threshold_ns),
        str(args.training_replays), str(args.max_lookahead),
        str(args.trigger_node), str(args.prefix_misses),
    ]
    print("# run:", " ".join(command))
    completed = subprocess.run(
        command, text=True, stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT, check=False,
    )
    if completed.returncode != 0:
        print(completed.stdout, end="", file=sys.stderr)
        raise RuntimeError(f"test exited with status {completed.returncode}")
    return completed.stdout


def parse_rows(text):
    section = None
    cases = {}
    for raw_line in text.splitlines():
        line = raw_line.strip()
        if line.startswith("[") and line.endswith("]"):
            section = line[1:-1]
            cases.setdefault(section, [])
            continue
        if section is None or not line or line.startswith("#"):
            continue
        fields = line.split()
        if len(fields) != 17:
            continue
        try:
            cases[section].append({
                "node": int(fields[0]),
                "distance": int(fields[1]),
                "target_node": int(fields[2]),
                "target_page": int(fields[3]),
                "target_line": int(fields[4]),
                "samples": int(fields[5]),
                "hits": int(fields[6]),
                "per_1000": float(fields[7]),
                "avg_latency_ns": float(fields[8]),
                "l1_events": int(fields[9]),
                "avg_l1_pmu": float(fields[10]),
                "l2_events": int(fields[11]),
                "avg_l2_pmu": float(fields[12]),
                "l1_active_samples": int(fields[13]),
                "l1_active_hits": int(fields[14]),
                "l2_active_samples": int(fields[15]),
                "l2_active_hits": int(fields[16]),
            })
        except ValueError:
            continue
    return cases


def paired_rows(cases):
    indexed = {
        case: {(row["node"], row["distance"]): row for row in rows}
        for case, rows in cases.items()
    }
    untrained = indexed.get("untrained", {})
    trained = indexed.get("trained", {})
    pairs = []
    for key in sorted(set(untrained) & set(trained)):
        urow = untrained[key]
        trow = trained[key]
        pairs.append({
            "node": key[0],
            "distance": key[1],
            "target_node": trow["target_node"],
            "cache_delta": trow["per_1000"] - urow["per_1000"],
            "latency_delta": (trow["avg_latency_ns"] -
                              urow["avg_latency_ns"]),
            "l1_delta": trow["avg_l1_pmu"] - urow["avg_l1_pmu"],
            "l2_delta": trow["avg_l2_pmu"] - urow["avg_l2_pmu"],
        })
    return pairs


def print_summary(cases):
    indexed = {
        case: {(row["node"], row["distance"]): row for row in rows}
        for case, rows in cases.items()
    }
    print("# lookahead u_hits/total t_hits/total delta_hits_per_1000 "
          "delta_avg_l1_pmu delta_avg_l2_pmu")
    maximum_distance = max(
        (row["distance"] for rows in indexed.values() for row in rows.values()),
        default=0,
    )
    for distance in range(1, maximum_distance + 1):
        grouped = {}
        for case in ("untrained", "trained"):
            rows = [row for (node, lookahead), row in indexed.get(case, {}).items()
                    if lookahead == distance]
            samples = sum(row["samples"] for row in rows)
            hits = sum(row["hits"] for row in rows)
            grouped[case] = {
                "samples": samples,
                "hits": hits,
                "l1": sum(row["l1_events"] for row in rows) /
                      (samples or 1),
                "l2": sum(row["l2_events"] for row in rows) /
                      (samples or 1),
            }
        u = grouped["untrained"]
        t = grouped["trained"]
        cache_delta = (1000.0 * t["hits"] / (t["samples"] or 1) -
                       1000.0 * u["hits"] / (u["samples"] or 1))
        print(f"{distance:2d} {u['hits']}/{u['samples']} "
              f"{t['hits']}/{t['samples']} {cache_delta:19.3f} "
              f"{t['l1'] - u['l1']:16.4f} "
              f"{t['l2'] - u['l2']:16.4f}")

    pairs = paired_rows(cases)
    strongest = sorted(
        (row for row in pairs
         if row["cache_delta"] >= 100 and row["latency_delta"] <= -10),
        key=lambda row: row["cache_delta"], reverse=True,
    )
    print("# strongest cache-supported targets")
    print("# node lookahead target_node delta_hits_per_1000 "
          "delta_latency_ns delta_l1 delta_l2")
    for row in strongest:
        print(f"{row['node']:3d} {row['distance']:2d} "
              f"{row['target_node']:3d} {row['cache_delta']:19.2f} "
              f"{row['latency_delta']:16.2f} {row['l1_delta']:8.4f} "
              f"{row['l2_delta']:8.4f}")

    print("# PMU-active trigger nodes and cache-supported lookaheads")
    print("# node avg_delta_l1 avg_delta_l2 supported_lookaheads")
    active = []
    for node in sorted({row["node"] for row in pairs}):
        rows = [row for row in pairs if row["node"] == node]
        avg_l1 = sum(row["l1_delta"] for row in rows) / len(rows)
        avg_l2 = sum(row["l2_delta"] for row in rows) / len(rows)
        supported = [
            row["distance"] for row in rows
            if row["cache_delta"] >= 100 and row["latency_delta"] <= -10
        ]
        if avg_l1 >= 0.1 or avg_l2 >= 0.1 or supported:
            active.append((node, avg_l1, avg_l2, supported))
    for node, avg_l1, avg_l2, supported in sorted(
            active, key=lambda row: max(row[1], row[2]), reverse=True):
        targets = ",".join(str(value) for value in supported) or "-"
        print(f"{node:3d} {avg_l1:12.4f} {avg_l2:12.4f} {targets}")

    trained_rows = indexed.get("trained", {})
    conditional = []
    for (node, distance), row in trained_rows.items():
        if row["l2_active_samples"] < 5:
            continue
        conditional.append((
            node, distance, row["target_node"],
            row["l2_active_samples"], row["l2_active_hits"],
            1000.0 * row["l2_active_hits"] / row["l2_active_samples"],
        ))
    print("# targets conditional on an L2 PMU event (at least 5 samples)")
    print("# node lookahead target_node l2_active_samples "
          "l2_active_hits conditional_hits_per_1000")
    for row in sorted(conditional, key=lambda value: value[5], reverse=True):
        print(f"{row[0]:3d} {row[1]:3d} {row[2]:3d} {row[3]:8d} "
              f"{row[4]:8d} {row[5]:25.2f}")


def plot(args, cases, output_path):
    if args.no_plot:
        return
    try:
        os.environ.setdefault("MPLCONFIGDIR", "/tmp/matplotlib-cache")
        import matplotlib
        matplotlib.use("Agg")
        import matplotlib.pyplot as plt
        import numpy as np
    except ImportError:
        print("# matplotlib/numpy unavailable; skip plot", file=sys.stderr)
        return

    pairs = paired_rows(cases)
    if not pairs:
        return
    node_count = max(row["node"] for row in pairs) + 1
    maximum_distance = max(row["distance"] for row in pairs)
    matrices = []
    for field in ("cache_delta", "l1_delta", "l2_delta"):
        matrix = np.full((maximum_distance, node_count), np.nan)
        for row in pairs:
            matrix[row["distance"] - 1, row["node"]] = row[field]
        matrices.append(matrix)

    figure, axes = plt.subplots(3, 1, figsize=(12, 8), sharex=True,
                                constrained_layout=True)
    labels = ("Delta cache hits / 1000", "Delta avg L1 PMU",
              "Delta avg L2 PMU")
    for axis, matrix, label in zip(axes, matrices, labels):
        image = axis.imshow(matrix, aspect="auto", origin="lower",
                            interpolation="nearest")
        axis.set_ylabel("Lookahead")
        axis.set_title(label)
        figure.colorbar(image, ax=axis, pad=0.01)
    axes[-1].set_xlabel("Trigger node")
    path = Path(args.plot) if args.plot else output_path.with_suffix(".png")
    path.parent.mkdir(parents=True, exist_ok=True)
    figure.savefig(path, dpi=250)
    plt.close(figure)
    print(f"# plot: {path}")


def main():
    args = parse_args()
    if not args.no_build:
        build(args)
    if args.no_run:
        return
    if platform.machine().lower() not in ("aarch64", "arm64"):
        raise RuntimeError("execution requires an AArch64 host")

    load_pmu(args)
    try:
        text = run(args)
    finally:
        module_script("module_unload.sh")

    output = (Path(args.output) if args.output else RESULT_DIR /
              f"tmp-x925-lookahead-{args.max_lookahead}-"
              f"r{args.training_replays}-node{args.trigger_node}-"
              f"prefix{args.prefix_misses}.txt")
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(text)
    print(text, end="")
    print(f"# output: {output}")
    cases = parse_rows(text)
    print_summary(cases)
    plot(args, cases, output)


if __name__ == "__main__":
    main()
