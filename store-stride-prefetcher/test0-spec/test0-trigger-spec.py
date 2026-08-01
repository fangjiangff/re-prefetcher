import argparse
import csv
import os
import re
import subprocess
import sys

ROOT_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
if ROOT_DIR not in sys.path:
    sys.path.insert(0, ROOT_DIR)

from cross_test_config import (
    apply_single_core_defaults,
    arch_choices,
)


BASE_DIR = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.join(BASE_DIR, "test0-trigger-spec.c")
OUT = os.path.join(ROOT_DIR, "bin", "test0-trigger-spec")
RESULT_DIR = os.path.join(ROOT_DIR, "res", "store-stride-spec")
RAW_DIR = os.path.join(RESULT_DIR, "raw")
PLOT_DIR = os.path.join(RESULT_DIR, "plots")

DEFAULT_TRAIN_TIMES = 15
DEFAULT_ROUNDS = 1
DEFAULT_TRIES = 9999
DEFAULT_SECRET_VALUE = 8
DEFAULT_SECRET_LEN = 1

RESULT_RE = re.compile(r"^results\[(\d+)\]\s*=\s*(-?\d+)\s*$")


def parse_args():
    parser = argparse.ArgumentParser(
        description=(
            "Compile and run the Spectre-style store-stride trigger PoC, "
            "then save per-array2-index latency results."
        )
    )
    parser.add_argument("--arch", required=True, choices=arch_choices())
    parser.add_argument("--core", type=int, default=None,
                        help="Override CPU core. Default comes from --arch.")
    parser.add_argument("--train-times", type=int, default=DEFAULT_TRAIN_TIMES,
                        help=f"Branch predictor training calls before attack. Default: {DEFAULT_TRAIN_TIMES}.")
    parser.add_argument("--rounds", type=int, default=DEFAULT_ROUNDS,
                        help=f"Outer PoC rounds macro. Default: {DEFAULT_ROUNDS}.")
    parser.add_argument("--tries", type=int, default=DEFAULT_TRIES,
                        help=f"Measurement attempts. Default: {DEFAULT_TRIES}.")
    parser.add_argument("--secret-value", type=int, default=DEFAULT_SECRET_VALUE,
                        help=f"Single-byte secret value. Default: {DEFAULT_SECRET_VALUE}.")
    parser.add_argument("--secret-len", type=int, default=DEFAULT_SECRET_LEN,
                        help=f"Bytes to read. Default: {DEFAULT_SECRET_LEN}.")
    parser.add_argument("--cc", default=os.environ.get("CC", "gcc"),
                        help="C compiler command. Default: $CC or gcc.")
    parser.add_argument("--output", default=None,
                        help="TSV output path. Default includes arch/core/secret.")
    parser.add_argument("--raw-output", default=None,
                        help="Raw program output path. Default includes arch/core/secret.")
    parser.add_argument("--plot-output", default=None,
                        help="Bar plot output path. Default includes arch/core/secret.")
    parser.add_argument("--plot-vmax", type=float, default=None)
    parser.add_argument("--plot-only", action="store_true",
                        help="Read an existing TSV and draw the plot.")
    parser.add_argument("--no-compile", action="store_true")
    parser.add_argument("--no-plot", action="store_true")
    parser.add_argument("--verbose", action="store_true",
                        help="Print raw C++ program output.")
    args = parser.parse_args()

    apply_single_core_defaults(args)

    if args.core < 0:
        parser.error("--core must be >= 0")
    if args.train_times < 1:
        parser.error("--train-times must be >= 1")
    if args.rounds < 1:
        parser.error("--rounds must be >= 1")
    if args.tries < 256:
        parser.error("--tries must be >= 256 so every array2 index is sampled")
    if args.secret_value < 0 or args.secret_value > 255:
        parser.error("--secret-value must be in [0, 255]")
    if args.secret_len < 1:
        parser.error("--secret-len must be >= 1")
    apply_default_paths(args)
    return args


def result_name(args):
    return (
        f"{args.arch}-core{args.core}-trigger-spec-storestride"
        f"-secret{args.secret_value}-train{args.train_times}"
        f"-rounds{args.rounds}-tries{args.tries}"
    )


def apply_default_paths(args):
    name = result_name(args)
    if args.output is None:
        args.output = os.path.join(RESULT_DIR, f"{name}.tsv")
    if args.raw_output is None:
        args.raw_output = os.path.join(RAW_DIR, f"{name}.txt")
    if args.plot_output is None:
        args.plot_output = os.path.join(PLOT_DIR, f"{name}.png")


def ensure_parent(path):
    parent = os.path.dirname(path)
    if parent:
        os.makedirs(parent, exist_ok=True)


def compile_test(args):
    os.makedirs(os.path.dirname(OUT), exist_ok=True)
    cmd = [
        args.cc,
        "-O0",
        "-static",
        f"-DTRAIN_TIMES={args.train_times}",
        f"-DROUNDS={args.rounds}",
        f"-DTRY_COUNT={args.tries}",
        f"-DSECRET_VALUE={args.secret_value}",
        f"-DSECRET_LEN={args.secret_len}",
        "-o",
        OUT,
        SRC,
    ]
    return subprocess.run(cmd).returncode


def run_binary(args):
    return subprocess.run(
        ["taskset", "-c", str(args.core), OUT],
        capture_output=True,
        text=True,
    )


def parse_output(text):
    rows = []
    seen = set()

    for line in text.splitlines():
        match = RESULT_RE.match(line.strip())
        if not match:
            continue
        index = int(match.group(1))
        avg_ns = int(match.group(2))
        rows.append({
            "index": index,
            "array2_offset": index * 64,
            "avg_ns": avg_ns,
        })
        seen.add(index)

    if not rows:
        raise ValueError("no results[i] rows found")
    if len(seen) != 256:
        raise ValueError(f"expected 256 result rows, found {len(seen)}")
    rows.sort(key=lambda row: row["index"])
    return rows


def write_tsv(path, rows):
    ensure_parent(path)
    with open(path, "w", newline="") as f:
        writer = csv.DictWriter(
            f,
            fieldnames=["index", "array2_offset", "avg_ns"],
            delimiter="\t",
        )
        writer.writeheader()
        writer.writerows(rows)


def read_tsv(path):
    rows = []
    with open(path, newline="") as f:
        reader = csv.DictReader(f, delimiter="\t")
        for row in reader:
            rows.append({
                "index": int(row["index"]),
                "array2_offset": int(row["array2_offset"]),
                "avg_ns": int(row["avg_ns"]),
            })
    if not rows:
        raise ValueError("no TSV rows found")
    return rows


def plot_result(path, rows, args):
    if not path:
        return
    try:
        import matplotlib.pyplot as plt
    except ModuleNotFoundError:
        print("Skipping plot: matplotlib is not installed.", file=sys.stderr)
        return

    ensure_parent(path)
    indices = [row["index"] for row in rows]
    values = [row["avg_ns"] for row in rows]

    secret = args.secret_value
    predicted = secret * 5
    by_index = {row["index"]: row["avg_ns"] for row in rows}

    fig, ax = plt.subplots(figsize=(13, 4.8))
    ax.plot(indices, values, color="#4C4C4C", linewidth=1.2, marker="o",
            markersize=2.4)
    for index, color, label in (
        (secret, "#D55E00", "secret"),
        (predicted, "#0072B2", "stride-predicted"),
    ):
        if index in by_index:
            ax.axvline(index, color=color, linestyle="--", linewidth=0.9,
                       alpha=0.75)
            ax.scatter([index], [by_index[index]], color=color, s=42,
                       zorder=3, label=f"{label}={index}")
    ax.legend(loc="upper right", frameon=False)
    ax.set_title(
        f"{args.arch} core {args.core}: speculative store-stride latency",
        loc="left",
        pad=8,
    )
    ax.set_xlabel("array2 cache-line index")
    ax.set_ylabel("average latency (ns)")
    ax.set_xlim(-1, 256)
    if args.plot_vmax is not None:
        ax.set_ylim(0, args.plot_vmax)
    ax.grid(axis="y", alpha=0.25)

    fig.tight_layout()
    fig.savefig(path, dpi=300)
    plt.close(fig)


def print_summary(rows, args):
    fastest = sorted(rows, key=lambda row: row["avg_ns"])[:10]
    predicted = args.secret_value * 5
    by_index = {row["index"]: row for row in rows}

    print("Fastest indices:")
    for row in fastest:
        markers = []
        if row["index"] == args.secret_value:
            markers.append("secret")
        if row["index"] == predicted:
            markers.append("stride-predicted")
        note = f" ({', '.join(markers)})" if markers else ""
        print(f"  {row['index']:3d}: {row['avg_ns']} ns{note}")

    if predicted in by_index:
        print(
            f"Predicted store-stride index {predicted}: "
            f"{by_index[predicted]['avg_ns']} ns"
        )


def main():
    args = parse_args()

    if args.plot_only:
        rows = read_tsv(args.output)
        print_summary(rows, args)
        if not args.no_plot:
            plot_result(args.plot_output, rows, args)
            print(f"Saved plot to {args.plot_output}")
        return 0

    print(
        f"arch={args.arch}, core={args.core}, secret={args.secret_value}, "
        f"train_times={args.train_times}, rounds={args.rounds}, tries={args.tries}"
    )

    if not args.no_compile:
        rc = compile_test(args)
        if rc != 0:
            print("Compile failed", file=sys.stderr)
            return rc

    run = run_binary(args)
    if args.verbose and run.stdout:
        print(run.stdout, end="")
    if run.stderr:
        print(run.stderr, end="", file=sys.stderr)
    if run.returncode != 0:
        print("Execution failed", file=sys.stderr)
        return run.returncode

    ensure_parent(args.raw_output)
    with open(args.raw_output, "w") as f:
        f.write(run.stdout)
    print(f"Saved raw output to {args.raw_output}")

    try:
        rows = parse_output(run.stdout)
    except ValueError as exc:
        print(f"Parse failed: {exc}", file=sys.stderr)
        return 1

    write_tsv(args.output, rows)
    print(f"Saved TSV to {args.output}")
    print_summary(rows, args)

    if not args.no_plot:
        plot_result(args.plot_output, rows, args)
        print(f"Saved plot to {args.plot_output}")

    return 0


if __name__ == "__main__":
    sys.exit(main())
