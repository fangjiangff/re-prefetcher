#!/usr/bin/env python3

import argparse
import math
import platform
import random
import shutil
import signal
import statistics
import subprocess
import sys
from pathlib import Path


HERE = Path(__file__).resolve().parent
REPO = HERE.parent
SRC = HERE / "test-bop-competition.c"
UTIL_SRC = REPO / "re0-stride-prefetcher" / "until.c"
BIN_DIR = REPO / "bin"
RESULT_DIR = HERE / "res"
ARCH_CONFIG = {
    "X925": {"core": 6, "threshold_ns": 80},
    "A725": {"core": 4, "threshold_ns": 120},
}
DEFAULT_SEED = 0x243F6A8885A308D3
INTEGER_COLUMNS = {
    "train_a", "train_b", "round_wins_a", "round_wins_b", "round_ties",
    "rounds_a_only", "rounds_b_only", "rounds_both", "rounds_neither",
    "rounds_mixed", "samples_a_only", "samples_b_only", "samples_both",
    "samples_neither",
}
FLOAT_COLUMNS = {
    "hits_a_per_1000", "hits_b_per_1000", "avg_ns_a", "avg_ns_b",
    "delta_a_minus_b",
}


def default_compiler():
    if platform.machine().lower() in ("aarch64", "arm64"):
        return "gcc"
    return shutil.which("aarch64-linux-gnu-gcc") or "gcc"


def inclusive_values(start, stop, step):
    return list(range(start, stop + 1, step))


def parse_args(argv=None):
    parser = argparse.ArgumentParser(
        description=(
            "Measure how many offset-B pairs replace an already trained "
            "offset A in a black-box BOP experiment."
        )
    )
    parser.add_argument("--arch", choices=ARCH_CONFIG, default="X925")
    parser.add_argument("--core", "--cpu", dest="core", type=int)
    parser.add_argument("--threshold-ns", type=int)
    parser.add_argument("--offset-a", type=int, default=13)
    parser.add_argument("--offset-b", type=int, default=16)
    parser.add_argument("--training-mode", choices=("paired", "compact"),
                        default="paired")
    parser.add_argument("--train-a-pairs", type=int, default=3000)
    parser.add_argument("--coarse-start", type=int, default=0)
    parser.add_argument("--coarse-stop", type=int, default=1200)
    parser.add_argument("--coarse-step", type=int, default=100)
    parser.add_argument("--fine-step", type=int, default=20)
    parser.add_argument("--repeats", type=int, default=5)
    parser.add_argument("--rounds", type=int, default=100)
    parser.add_argument("--trigger-batch", type=int, default=64)
    parser.add_argument("--old-max", type=float, default=250.0)
    parser.add_argument("--new-min", type=float, default=900.0)
    parser.add_argument("--confirmation-points", type=int, default=2)
    parser.add_argument("--seed", type=lambda value: int(value, 0),
                        default=DEFAULT_SEED)
    parser.add_argument("--wait-nops", type=int, default=2000)
    parser.add_argument("--cc", default=default_compiler())
    parser.add_argument("--output", type=Path)
    parser.add_argument("--describe", action="store_true")
    parser.add_argument("--no-build", action="store_true")
    parser.add_argument("--no-run", action="store_true")
    args = parser.parse_args(argv)

    config = ARCH_CONFIG[args.arch]
    if args.core is None:
        args.core = config["core"]
    if args.threshold_ns is None:
        args.threshold_ns = config["threshold_ns"]

    positive = (
        args.threshold_ns, args.train_a_pairs, args.coarse_step,
        args.fine_step, args.repeats, args.rounds, args.trigger_batch,
    )
    if args.core < 0 or args.coarse_start < 0 or args.wait_nops < 0:
        parser.error("core, coarse-start, and wait-nops must be non-negative")
    if any(value <= 0 for value in positive):
        parser.error("threshold, training, steps, repeats, rounds, and batch must be positive")
    if args.coarse_stop < args.coarse_start:
        parser.error("coarse-stop must not be smaller than coarse-start")
    if args.coarse_start != 0:
        parser.error("coarse-start must be zero to validate the old best offset")
    if (args.coarse_stop - args.coarse_start) % args.coarse_step:
        parser.error("coarse range must end on the coarse-step grid")
    if args.coarse_step % args.fine_step:
        parser.error("coarse-step must be divisible by fine-step")
    if not 1 <= args.offset_a <= 63 or not 1 <= args.offset_b <= 63:
        parser.error("offsets must be in cache lines 1..63")
    if args.offset_a == args.offset_b:
        parser.error("offset-a and offset-b must be distinct")
    if math.gcd(args.offset_a, args.offset_b) != 1:
        parser.error("offset-a and offset-b must be coprime")
    if not 0 <= args.old_max <= 1000 or not 0 <= args.new_min <= 1000:
        parser.error("old-max and new-min must be in 0..1000")
    if args.old_max >= args.new_min:
        parser.error("old-max must be smaller than new-min")
    if args.rounds < 2 or args.rounds % 2:
        parser.error("rounds must be even and at least two for isolated probes")
    if args.confirmation_points < 0:
        parser.error("confirmation-points must be non-negative")
    if not 0 <= args.seed <= (1 << 64) - 1:
        parser.error("seed must fit in an unsigned 64-bit integer")

    maximum_pages = (
        args.train_a_pairs + (args.train_a_pairs + 6) // 7
        + args.coarse_stop + (args.coarse_stop + 6) // 7
        + args.trigger_batch
    )
    if maximum_pages > 8192:
        parser.error("training and trigger sets exceed the C program's 8192-page pool")
    return args


def print_schedule(args):
    print(
        f"# offset_a={args.offset_a} offset_b={args.offset_b} "
        f"training_mode={args.training_mode} train_a_pairs={args.train_a_pairs} "
        f"repeats={args.repeats}"
    )
    print("stage\ttrain_b_pairs")
    for pairs in inclusive_values(
        args.coarse_start, args.coarse_stop, args.coarse_step
    ):
        print(f"coarse\t{pairs}")
    print("# fine stage is selected from the observed transition band")


def build(args):
    BIN_DIR.mkdir(parents=True, exist_ok=True)
    binary = BIN_DIR / "test-bop-transition-cache"
    command = [
        args.cc, "-std=gnu11", "-O2", "-Wall", "-Wextra", "-Werror",
        "-Wno-unused-result", "-march=armv8.5-a+predres",
        "-DENABLE_CPP_RCTX=1", f"-DPREFETCH_WAIT_NOPS={args.wait_nops}",
        str(SRC), str(UTIL_SRC), "-o", str(binary),
    ]
    print("# build:", " ".join(command))
    subprocess.run(command, check=True)
    return binary


def parse_transition_result(output):
    lines = output.splitlines()
    header_index = next(
        (index for index, line in enumerate(lines) if line.startswith("case\t")),
        None,
    )
    if header_index is None:
        raise RuntimeError(f"competition output has no table header:\n{output}")
    names = lines[header_index].split("\t")
    for line in lines[header_index + 1:]:
        values = line.split("\t")
        if values[0] != "transition_a_then_b":
            continue
        if len(names) != len(values):
            raise RuntimeError("transition output row has the wrong column count")
        row = dict(zip(names, values))
        for name in INTEGER_COLUMNS & row.keys():
            row[name] = int(row[name])
        for name in FLOAT_COLUMNS & row.keys():
            row[name] = float(row[name])
        return row
    raise RuntimeError(f"competition output has no transition row:\n{output}")


def experiment_seed(base_seed, stage, train_b, repeat):
    stage_tag = 0xC0A25E if stage == "coarse" else 0xF1AE
    value = base_seed ^ stage_tag ^ (train_b * 0x9E3779B97F4A7C15)
    value ^= repeat * 0xBF58476D1CE4E5B9
    return value & ((1 << 64) - 1)


def run_one(args, binary, stage, train_b, repeat):
    seed = experiment_seed(args.seed, stage, train_b, repeat)
    command = [
        str(binary), "--only-transition", "--isolated-probes",
        "--training-mode", args.training_mode,
        "--cpu", str(args.core),
        "--rounds", str(args.rounds), "--threshold-ns", str(args.threshold_ns),
        "--train-pairs", "1", "--trigger-batch", str(args.trigger_batch),
        "--offset-a", str(args.offset_a), "--offset-b", str(args.offset_b),
        "--transition-a-pairs", str(args.train_a_pairs),
        "--transition-b-pairs", str(train_b), "--seed", str(seed),
    ]
    print("# run:", " ".join(command))
    completed = subprocess.run(
        command, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT
    )
    if completed.returncode != 0:
        reason = (
            signal.Signals(-completed.returncode).name
            if completed.returncode < 0
            else f"status {completed.returncode}"
        )
        raise RuntimeError(f"transition run failed with {reason}:\n{completed.stdout}")
    row = parse_transition_result(completed.stdout)
    row.update(stage=stage, repeat=repeat, seed=seed)
    return row


def summarize_point(records, old_max, new_min):
    if not records:
        raise ValueError("cannot summarize an empty point")
    train_b = records[0]["train_b"]
    if any(record["train_b"] != train_b for record in records):
        raise ValueError("all records in a point must have the same train_b")
    a_hits = [record["hits_a_per_1000"] for record in records]
    b_hits = [record["hits_b_per_1000"] for record in records]
    return {
        "train_b": train_b,
        "repeats": len(records),
        "mean_hits_a_per_1000": statistics.fmean(a_hits),
        "min_hits_a_per_1000": min(a_hits),
        "max_hits_a_per_1000": max(a_hits),
        "mean_hits_b_per_1000": statistics.fmean(b_hits),
        "min_hits_b_per_1000": min(b_hits),
        "max_hits_b_per_1000": max(b_hits),
        "crossover": statistics.fmean(b_hits) > statistics.fmean(a_hits),
        "dominant": all(b > a for a, b in zip(a_hits, b_hits)),
        "stable_raw": all(a <= old_max and b >= new_min
                          for a, b in zip(a_hits, b_hits)),
    }


def summarize_records(records, old_max, new_min):
    by_pairs = {}
    for record in records:
        by_pairs.setdefault(record["train_b"], []).append(record)
    return [
        summarize_point(by_pairs[pairs], old_max, new_min)
        for pairs in sorted(by_pairs)
    ]


def find_boundaries(points, confirmation_points=2):
    ordered = sorted(points, key=lambda point: point["train_b"])

    def first(flag):
        return next((point["train_b"] for point in ordered if point[flag]), None)

    stable = None
    width = confirmation_points + 1
    for index in range(0, len(ordered) - width + 1):
        window = ordered[index:index + width]
        if all(point["stable_raw"] for point in window):
            stable = window[0]["train_b"]
            break
    return {
        "crossover_pairs": first("crossover"),
        "dominant_pairs": first("dominant"),
        "stable_replacement_pairs": stable,
    }


def old_best_established(points, old_max, new_min):
    baseline = next((point for point in points if point["train_b"] == 0), None)
    return bool(
        baseline
        and baseline["min_hits_a_per_1000"] >= new_min
        and baseline["max_hits_b_per_1000"] <= old_max
    )


def validated_boundaries(points, confirmation_points, old_max, new_min):
    boundaries = find_boundaries(points, confirmation_points)
    established = old_best_established(points, old_max, new_min)
    boundaries["old_best_established"] = established
    if not established:
        boundaries.update(
            crossover_pairs=None,
            dominant_pairs=None,
            stable_replacement_pairs=None,
        )
    return boundaries


def choose_fine_values(coarse, start, stop, coarse_step, fine_step):
    crossover = next(
        (point["train_b"] for point in coarse if point["crossover"]), stop
    )
    stable = next(
        (point["train_b"] for point in coarse if point["stable_raw"]), None
    )
    lower = max(start, crossover - coarse_step)
    upper = stop if stable is None else min(stop, stable + 2 * coarse_step)
    return inclusive_values(lower, upper, fine_step)


def run_stage(args, binary, stage, values):
    jobs = [(pairs, repeat) for pairs in values
            for repeat in range(1, args.repeats + 1)]
    random.Random(args.seed ^ (0xC0A25E if stage == "coarse" else 0xF1AE)).shuffle(jobs)
    return [run_one(args, binary, stage, pairs, repeat)
            for pairs, repeat in jobs]


def output_paths(args):
    if args.output:
        raw = args.output
    else:
        RESULT_DIR.mkdir(parents=True, exist_ok=True)
        raw = RESULT_DIR / (
            f"bop-{args.arch}-cpu{args.core}-off{args.offset_a}-to-"
            f"off{args.offset_b}-transition.tsv"
        )
        run = 2
        original = raw
        summary = raw.with_name(f"{raw.stem}-summary{raw.suffix}")
        while raw.exists() or summary.exists():
            raw = original.with_name(f"{original.stem}-run{run}{original.suffix}")
            summary = raw.with_name(f"{raw.stem}-summary{raw.suffix}")
            run += 1
    summary = raw.with_name(f"{raw.stem}-summary{raw.suffix}")
    return raw, summary


def metadata_lines(args):
    return [
        "# BOP offset replacement black-box experiment",
        f"# arch={args.arch} core={args.core} offset_a={args.offset_a} "
        f"offset_b={args.offset_b} training_mode={args.training_mode} "
        f"train_a_pairs={args.train_a_pairs} "
        f"repeats={args.repeats} rounds={args.rounds} "
        f"trigger_batch={args.trigger_batch} threshold_ns={args.threshold_ns} "
        f"seed={args.seed}",
        f"# coarse={args.coarse_start}:{args.coarse_stop}:{args.coarse_step} "
        f"fine_step={args.fine_step} old_max={args.old_max:g} "
        f"new_min={args.new_min:g} confirmation_points={args.confirmation_points}",
        "# every row is a fresh process: A is retrained before B; winner is inferred from cache probes",
    ]


def save_results(args, records, points, boundaries):
    raw_path, summary_path = output_paths(args)
    raw_path.parent.mkdir(parents=True, exist_ok=True)
    raw_columns = [
        "stage", "repeat", "seed", "train_a", "train_b",
        "hits_a_per_1000", "hits_b_per_1000", "avg_ns_a", "avg_ns_b",
        "round_wins_a", "round_wins_b", "round_ties", "rounds_a_only",
        "rounds_b_only", "rounds_both", "rounds_neither", "rounds_mixed",
        "samples_a_only", "samples_b_only", "samples_both", "samples_neither",
        "winner", "delta_a_minus_b",
    ]
    raw_lines = metadata_lines(args) + ["\t".join(raw_columns)]
    for row in sorted(records, key=lambda item: (item["train_b"], item["repeat"])):
        raw_lines.append("\t".join(str(row[column]) for column in raw_columns))
    raw_path.write_text("\n".join(raw_lines) + "\n")

    summary_columns = [
        "train_b", "repeats", "mean_hits_a_per_1000", "min_hits_a_per_1000",
        "max_hits_a_per_1000", "mean_hits_b_per_1000", "min_hits_b_per_1000",
        "max_hits_b_per_1000", "crossover", "dominant", "stable_raw",
    ]
    boundary_text = " ".join(
        f"{name}={value if value is not None else 'not_observed'}"
        for name, value in boundaries.items()
    )
    summary_lines = metadata_lines(args) + [f"# {boundary_text}", "\t".join(summary_columns)]
    for point in points:
        summary_lines.append("\t".join(str(point[column]) for column in summary_columns))
    summary_path.write_text("\n".join(summary_lines) + "\n")
    print(f"# raw output: {raw_path}")
    print(f"# summary output: {summary_path}")


def main(argv=None):
    args = parse_args(argv)
    if args.describe:
        print_schedule(args)
        return 0
    binary = BIN_DIR / "test-bop-transition-cache" if args.no_build else build(args)
    if args.no_run:
        return 0
    if platform.machine().lower() not in ("aarch64", "arm64"):
        raise RuntimeError("execution requires an AArch64 host; use --no-run")

    coarse_values = inclusive_values(
        args.coarse_start, args.coarse_stop, args.coarse_step
    )
    records = run_stage(args, binary, "coarse", coarse_values)
    coarse_points = summarize_records(records, args.old_max, args.new_min)
    fine_values = choose_fine_values(
        coarse_points, args.coarse_start, args.coarse_stop,
        args.coarse_step, args.fine_step,
    )
    coarse_set = set(coarse_values)
    records.extend(run_stage(
        args, binary, "fine", [value for value in fine_values
                               if value not in coarse_set]
    ))
    points = summarize_records(records, args.old_max, args.new_min)
    boundaries = validated_boundaries(
        points, args.confirmation_points, args.old_max, args.new_min
    )
    save_results(args, records, points, boundaries)
    print("# boundaries:", " ".join(
        f"{name}={value if value is not None else 'not_observed'}"
        for name, value in boundaries.items()
    ))
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (OSError, RuntimeError, subprocess.CalledProcessError) as error:
        print(f"error: {error}", file=sys.stderr)
        sys.exit(1)
