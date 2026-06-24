import argparse
import csv
import os
import subprocess
import sys

from cross_test_config import (
    ARCH_CONFIG,
    apply_single_core_defaults,
    apply_threshold_defaults,
    arch_choices,
)


BASE_DIR = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.join(BASE_DIR, "test2-associativity.c")
OUT = os.path.join(BASE_DIR, "bin", "test2-associativity")

DEFAULT_STORE_PC = "0x500000120"
DEFAULT_POOL_MIB = 16384
DEFAULT_ROUNDS = 1000
DEFAULT_MATCH_BITS = 4
DEFAULT_GUESSES = "4,8,16,32,64"
DEFAULT_STRIDE_LINES = 5


def parse_args():
    parser = argparse.ArgumentParser(
        description="Measure store-stride prefetcher set associativity using PA-colored pages."
    )
    parser.add_argument("--arch", required=True, choices=arch_choices())
    parser.add_argument("--core", type=int, default=None)
    parser.add_argument("--store-pc", default=DEFAULT_STORE_PC)
    parser.add_argument("--pool-mib", type=int, default=DEFAULT_POOL_MIB,
                        help="Anonymous memory pool size. Default: 16384 MiB.")
    parser.add_argument("--rounds", type=int, default=DEFAULT_ROUNDS)
    parser.add_argument("--match-bits", type=int, default=DEFAULT_MATCH_BITS,
                        help="Require identical PA bits [12:12+match_bits-1].")
    parser.add_argument("--guesses", default=DEFAULT_GUESSES,
                        help="Comma-separated guessed ways. Each guess uses N competitors plus one victim.")
    parser.add_argument("--stride", type=int, default=DEFAULT_STRIDE_LINES)
    parser.add_argument("--train-accesses", type=int, default=None,
                        help="Default is cross_test_config[arch]['accesses']['store'] - 1.")
    parser.add_argument("--threshold-ns", type=int, default=None)
    parser.add_argument("--cc", default=os.environ.get("CC", "gcc"))
    parser.add_argument("--output", default=None)
    parser.add_argument("--raw-output", default=None)
    parser.add_argument("--plot", default=None)
    parser.add_argument("--no-compile", action="store_true")
    args = parser.parse_args()

    args.access = "store"
    apply_single_core_defaults(args)
    if args.train_accesses is None:
        args.train_accesses = ARCH_CONFIG[args.arch]["accesses"]["store"] - 1
    apply_threshold_defaults(args)

    guesses = []
    for item in args.guesses.split(","):
        try:
            value = int(item, 0)
        except ValueError:
            parser.error("--guesses must be comma-separated integers")
        if value < 0:
            parser.error("--guesses values must be >= 0")
        guesses.append(value)
    if not guesses:
        parser.error("--guesses must not be empty")

    if args.core < 0:
        parser.error("--core must be >= 0")
    if args.pool_mib < 1:
        parser.error("--pool-mib must be >= 1")
    if args.rounds < 1:
        parser.error("--rounds must be >= 1")
    if args.match_bits < 1 or args.match_bits > 16:
        parser.error("--match-bits must be in [1, 16]")
    if args.stride < 1:
        parser.error("--stride must be >= 1")
    if args.train_accesses < 1:
        parser.error("--train-accesses must be >= 1")
    if args.threshold_ns < 1:
        parser.error("--threshold-ns must be >= 1")

    predicted_line = (args.train_accesses + 2) * args.stride
    if predicted_line >= 64:
        parser.error("(train_accesses + 2) * stride must keep probe line inside one 4KB page")

    return args


args = parse_args()
result_dir = os.path.join(BASE_DIR, "res", "associativity")
raw_dir = os.path.join(result_dir, "raw")
plot_dir = os.path.join(result_dir, "plots")


def micro_arch_name():
    guesses = args.guesses.replace(",", "-")
    return (
        f"{args.arch}-core{args.core}-assoc"
        f"-stride{args.stride}-train{args.train_accesses}"
        f"-match{args.match_bits}-pool{args.pool_mib}m-guesses{guesses}"
    )


def tsv_path():
    if args.output:
        return args.output
    return os.path.join(result_dir, f"{micro_arch_name()}.tsv")


def raw_path():
    if args.raw_output:
        return args.raw_output
    return os.path.join(raw_dir, f"{micro_arch_name()}.txt")


def plot_path():
    if args.plot:
        return args.plot
    return os.path.join(plot_dir, f"{micro_arch_name()}.png")


def ensure_dirs():
    os.makedirs(os.path.dirname(OUT), exist_ok=True)
    for path in (tsv_path(), raw_path(), plot_path()):
        directory = os.path.dirname(path)
        if directory:
            os.makedirs(directory, exist_ok=True)


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
        os.path.join(BASE_DIR, "until.c"),
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
            str(args.pool_mib),
            str(args.rounds),
            str(args.match_bits),
            args.guesses,
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
        if len(fields) != 6:
            print(f"Skipping unexpected output row: {line}", file=sys.stderr)
            continue
        try:
            guess_ways = int(fields[0])
            competitors = int(fields[1])
            entries_same_color = int(fields[2])
            color = fields[3]
            avg_ns = int(fields[4])
            probes = int(fields[5])
        except ValueError:
            print(f"Skipping non-numeric output row: {line}", file=sys.stderr)
            continue
        rows.append({
            "guess_ways": guess_ways,
            "competitors": competitors,
            "entries_same_color": entries_same_color,
            "color": color,
            "avg_ns": avg_ns,
            "prefetched": "yes" if avg_ns <= args.threshold_ns else "no",
            "threshold_ns": args.threshold_ns,
            "probes": probes,
        })
    return rows


def write_tsv(rows):
    with open(tsv_path(), "w", newline="") as f:
        writer = csv.DictWriter(
            f,
            fieldnames=[
                "guess_ways",
                "competitors",
                "entries_same_color",
                "color",
                "avg_ns",
                "prefetched",
                "threshold_ns",
                "probes",
            ],
            delimiter="\t",
        )
        writer.writeheader()
        writer.writerows(rows)


def plot_rows(rows):
    try:
        import matplotlib.pyplot as plt
        from matplotlib.ticker import MaxNLocator
    except ModuleNotFoundError as exc:
        print(f"Skipping plot: missing Python package '{exc.name}'.")
        return

    xs = [row["entries_same_color"] for row in rows]
    ys = [row["avg_ns"] for row in rows]
    colors = ["#D55E00" if row["prefetched"] == "no" else "#0072B2"
              for row in rows]

    fig, ax = plt.subplots(figsize=(7.5, 4))
    ax.scatter(xs, ys, s=48, marker="D", color=colors, edgecolor="black", linewidth=0.35)
    ax.axhline(args.threshold_ns, color="black", linestyle="--", linewidth=1.0)
    ax.set_title(f"{args.arch} core {args.core}: store-stride associativity", loc="left")
    ax.set_xlabel("Same-color entries primed (victim + competitors)")
    ax.set_ylabel("Victim predicted-line latency (ns)")
    ax.grid(axis="y", alpha=0.25)
    ax.xaxis.set_major_locator(MaxNLocator(integer=True))
    if xs:
        ax.set_xlim(0, max(xs) + 4)
    fig.tight_layout()
    fig.savefig(plot_path(), dpi=300)
    plt.close(fig)
    print(f"Saved plot to {plot_path()}")


def first_eviction(rows):
    for row in rows:
        if row["prefetched"] == "no":
            return row
    return None


def main():
    ensure_dirs()
    print(
        f"arch={args.arch}, core={args.core}, stride={args.stride}, "
        f"train_accesses={args.train_accesses}, pool_mib={args.pool_mib}, "
        f"rounds={args.rounds}, match_bits={args.match_bits}, "
        f"guesses={args.guesses}, threshold={args.threshold_ns} ns"
    )
    print(
        "config default train_accesses="
        f"{ARCH_CONFIG[args.arch]['accesses']['store'] - 1}"
    )

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

    evicted = first_eviction(rows)
    if evicted:
        green = "\033[32m"
        reset = "\033[0m"
        print(
            green +
            "first associativity guess that evicted victim: "
            f"guess_ways={evicted['guess_ways']} "
            f"entries_same_color={evicted['entries_same_color']} "
            f"avg_ns={evicted['avg_ns']}" +
            reset
        )
    else:
        print("victim stayed below threshold for all associativity guesses")

    plot_rows(rows)
    return 0


if __name__ == "__main__":
    sys.exit(main())
