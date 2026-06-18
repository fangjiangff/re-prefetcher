import argparse
import csv
import os
import subprocess
import sys

from cross_test_config import (
    ARCH_CONFIG,
    apply_single_core_defaults,
    apply_threshold_defaults,
    apply_train_access_defaults,
    arch_choices,
)


BASE_DIR = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.join(BASE_DIR, "test2-entry-interleaved.c")
UTIL_SRC = os.path.join(BASE_DIR, "until.c")
OUT = os.path.join(BASE_DIR, "bin", "test2-entry-interleaved")

DEFAULT_STORE_PC = "0x500000120"
DEFAULT_STREAM_BUFFER = "0x600000000"
DEFAULT_STRIDE_LINES = 5
DEFAULT_ACTIVE_PAGES = 512
DEFAULT_ROUNDS = 1000
DEFAULT_PAGE_STEP = 1


def parse_args():
    parser = argparse.ArgumentParser(
        description="Measure store-stride prefetcher entry capacity with interleaved 4KB-page streams."
    )
    parser.add_argument("--arch", required=True, choices=arch_choices())
    parser.add_argument("--core", type=int, default=None,
                        help="Override CPU core. Default is selected from --arch.")
    parser.add_argument("--store-pc", default=DEFAULT_STORE_PC,
                        help=f"Fixed VA for the same-PC store gadget. Default: {DEFAULT_STORE_PC}")
    parser.add_argument("--stream-buffer", default=DEFAULT_STREAM_BUFFER,
                        help=f"Fixed VA for stream pages. Default: {DEFAULT_STREAM_BUFFER}")
    parser.add_argument("--stride", type=int, default=DEFAULT_STRIDE_LINES,
                        help=f"Stride in cache lines. Default: {DEFAULT_STRIDE_LINES}")
    parser.add_argument("--train-accesses", type=int, default=None,
                        help="Number of same-page stores needed to critically trigger prefetch. "
                             "Default is cross_test_config[arch]['train_accesses']['store'].")
    parser.add_argument("--active-pages", type=int, default=None,
                        help=f"Fixed number of simultaneously active 4KB pages. Default: {DEFAULT_ACTIVE_PAGES}")
    parser.add_argument("--max-pages", type=int, default=None,
                        help="Deprecated alias for --active-pages.")
    parser.add_argument("--rounds", type=int, default=DEFAULT_ROUNDS,
                        help=f"Rounds per active_pages setting. Default: {DEFAULT_ROUNDS}")
    parser.add_argument("--page-step", type=int, default=DEFAULT_PAGE_STEP,
                        help="Distance between stream pages in 4KB pages. Default: 1")
    parser.add_argument("--threshold-ns", type=int, default=None,
                        help="Latency threshold used for summary classification. "
                             "Default is selected from --arch.")
    parser.add_argument("--cc", default=os.environ.get("CC", "gcc"))
    parser.add_argument("--output", default=None,
                        help="TSV output path. Default is derived from arch/core/config.")
    parser.add_argument("--plot-only", action="store_true")
    parser.add_argument("--no-plot", action="store_true")
    parser.add_argument("--no-compile", action="store_true")
    args = parser.parse_args()

    args.access = "store"
    apply_single_core_defaults(args)
    apply_train_access_defaults(args)
    apply_threshold_defaults(args)

    if args.active_pages is None:
        args.active_pages = args.max_pages if args.max_pages is not None else DEFAULT_ACTIVE_PAGES

    if args.core < 0:
        parser.error("--core must be >= 0")
    if args.stride < 1:
        parser.error("--stride must be >= 1")
    if args.train_accesses < 1:
        parser.error("--train-accesses must be >= 1")
    if args.active_pages < 1:
        parser.error("--active-pages must be >= 1")
    if args.rounds < 1:
        parser.error("--rounds must be >= 1")
    if args.page_step < 1:
        parser.error("--page-step must be >= 1")
    if args.threshold_ns < 1:
        parser.error("--threshold-ns must be >= 1")
    if args.train_accesses * args.stride >= 64:
        parser.error("train_accesses * stride must keep predicted line inside one 4KB page")

    return args


args = parse_args()
result_dir = os.path.join(BASE_DIR, "res", "entry-interleaved")
plot_dir = os.path.join(BASE_DIR, "res", "barplots")
raw_dir = os.path.join(result_dir, "raw")


def micro_arch_name():
    return (
        f"{args.arch}-core{args.core}-entry-interleaved"
        f"-stride{args.stride}-train{args.train_accesses}"
        f"-active{args.active_pages}-step{args.page_step}"
    )


def tsv_path():
    if args.output:
        return args.output
    return os.path.join(result_dir, f"{micro_arch_name()}.tsv")


def raw_path():
    return os.path.join(raw_dir, f"{micro_arch_name()}.txt")


def plot_path():
    return os.path.join(plot_dir, f"{micro_arch_name()}-avg_ns.png")


def ensure_dirs():
    os.makedirs(os.path.dirname(OUT), exist_ok=True)
    os.makedirs(result_dir, exist_ok=True)
    os.makedirs(raw_dir, exist_ok=True)
    os.makedirs(plot_dir, exist_ok=True)
    output_dir = os.path.dirname(tsv_path())
    if output_dir:
        os.makedirs(output_dir, exist_ok=True)


def compile_test():
    compile_cmd = [
        args.cc,
        "-std=gnu11",
        "-O0",
        "-static",
        f"-DSTRIDE_LINES={args.stride}",
        f"-DTRAIN_ACCESSES={args.train_accesses}",
        "-o",
        OUT,
        SRC,
        UTIL_SRC,
    ]
    return subprocess.run(compile_cmd).returncode


def run_binary():
    return subprocess.run(
        [
            "taskset",
            "-c",
            str(args.core),
            OUT,
            args.store_pc,
            args.stream_buffer,
            str(args.active_pages),
            str(args.rounds),
            str(args.page_step),
        ],
        capture_output=True,
        text=True,
    )


def parse_output(output):
    rows = []

    for line in output.splitlines():
        stripped = line.strip()
        if not stripped or stripped.startswith("#"):
            continue

        fields = stripped.split()
        if len(fields) != 4:
            print(f"Skipping unexpected output row: {line}", file=sys.stderr)
            continue

        try:
            active_pages = int(fields[0])
            stream = int(fields[1])
            avg_latency_ns = int(fields[2])
            probes = int(fields[3])
        except ValueError:
            print(f"Skipping non-numeric output row: {line}", file=sys.stderr)
            continue

        rows.append({
            "active_pages": active_pages,
            "stream": stream,
            "avg_latency_ns": avg_latency_ns,
            "prefetched": "yes" if avg_latency_ns <= args.threshold_ns else "no",
            "threshold_ns": args.threshold_ns,
            "probes": probes,
        })

    return rows


def write_tsv(rows):
    with open(tsv_path(), "w", newline="") as f:
        writer = csv.DictWriter(
            f,
            fieldnames=[
                "active_pages",
                "stream",
                "avg_latency_ns",
                "prefetched",
                "threshold_ns",
                "probes",
            ],
            delimiter="\t",
        )
        writer.writeheader()
        writer.writerows(rows)


def read_tsv():
    rows = []
    with open(tsv_path(), newline="") as f:
        reader = csv.DictReader(f, delimiter="\t")
        for row in reader:
            rows.append({
                "active_pages": int(row["active_pages"]),
                "stream": int(row["stream"]),
                "avg_latency_ns": int(row["avg_latency_ns"]),
                "prefetched": row["prefetched"],
                "threshold_ns": int(row["threshold_ns"]),
                "probes": int(row["probes"]),
            })
    return rows


def first_failure(rows):
    for row in rows:
        if row["prefetched"] == "no":
            return row
    return None


def plot_rows(rows):
    if args.no_plot:
        return

    try:
        import matplotlib.pyplot as plt
    except ModuleNotFoundError as exc:
        print(f"Skipping plot: missing Python package '{exc.name}'.")
        return

    xs = [row["stream"] for row in rows]
    ys = [row["avg_latency_ns"] for row in rows]
    colors = ["#0072B2" if row["prefetched"] == "yes" else "#D55E00"
              for row in rows]

    width = min(max(len(rows) / 16, 8), 18)
    fig, ax = plt.subplots(1, 1, figsize=(width, 4))
    ax.bar(xs, ys, color=colors, edgecolor="black", linewidth=0.2, width=0.85)
    ax.axhline(args.threshold_ns, color="black", linestyle="--", linewidth=0.9)
    ax.set_title(
        f"{args.arch} core {args.core}: interleaved store-stride entry test",
        loc="left",
        pad=4,
    )
    ax.set_xlabel("Stream / page index")
    ax.set_ylabel("Per-stream average predicted-line reload ns")
    ax.grid(axis="y", alpha=0.25)
    ax.set_xlim(-1, max(xs) + 1 if xs else args.active_pages)
    ax.set_ylim(0, max(300, max(ys) * 1.05 if ys else 300))
    fig.tight_layout()
    fig.savefig(plot_path(), dpi=300)
    plt.close(fig)
    print(f"Saved plot to {plot_path()}")


def main():
    ensure_dirs()

    print(
        f"arch={args.arch}, core={args.core}, stride={args.stride}, "
        f"train_accesses={args.train_accesses}, active_pages={args.active_pages}, "
        f"rounds={args.rounds}, threshold={args.threshold_ns} ns"
    )
    print(
        "config default train_accesses="
        f"{ARCH_CONFIG[args.arch]['train_accesses']['store']}"
    )

    if args.plot_only:
        if not os.path.exists(tsv_path()):
            print(f"Error: TSV result '{tsv_path()}' not found.", file=sys.stderr)
            return 1
        rows = read_tsv()
    else:
        if not args.no_compile and compile_test() != 0:
            print("Compile failed", file=sys.stderr)
            return 1

        run = run_binary()
        if run.stdout:
            print(run.stdout, end="")
        if run.stderr:
            print(run.stderr, end="", file=sys.stderr)
        if run.returncode != 0:
            print("Execution failed", file=sys.stderr)
            return run.returncode

        with open(raw_path(), "w") as f:
            f.write(run.stdout)

        rows = parse_output(run.stdout)
        if not rows:
            print("No result rows parsed", file=sys.stderr)
            return 1
        write_tsv(rows)
        print(f"Saved TSV to {tsv_path()}")
        print(f"Saved raw output to {raw_path()}")

    failed = first_failure(rows)
    if failed:
        print(
            "first non-prefetched point: "
            f"active_pages={failed['active_pages']} stream={failed['stream']} "
            f"avg_latency_ns={failed['avg_latency_ns']}"
        )
    else:
        print("all tested streams stayed below the threshold")

    plot_rows(rows)
    return 0


if __name__ == "__main__":
    sys.exit(main())
