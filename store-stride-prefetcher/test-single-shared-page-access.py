import argparse
import csv
import os
import subprocess
import sys


BASE_DIR = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.join(BASE_DIR, "test-single-shared-page-access.c")
UTIL_SRC = os.path.join(BASE_DIR, "until.c")
OUT = os.path.join(BASE_DIR, "bin", "test-single-shared-page-access")

DEFAULT_ARCH = "A78"
DEFAULT_CORE = 4
DEFAULT_ACCESS_LINE = 24
DEFAULT_ROUNDS = 4000
DEFAULT_RELOAD_STEP = 17
DEFAULT_WAIT_NOPS = 1000
DEFAULT_HIT_THRESHOLD_NS = 120

ROLE_COLORS = {
    "accessed": "#D55E00",
    "prefetched": "#56B4E9",
    "cache_miss": "#BDBDBD",
}


def parse_args():
    parser = argparse.ArgumentParser(
        description="Compile, run, and plot single shared-page access reload test."
    )
    parser.add_argument("--arch", default=DEFAULT_ARCH)
    parser.add_argument("--core", type=int, default=DEFAULT_CORE)
    parser.add_argument("--access-line", type=int, default=DEFAULT_ACCESS_LINE)
    parser.add_argument("--rounds", type=int, default=DEFAULT_ROUNDS)
    parser.add_argument("--reload-step", type=int, default=DEFAULT_RELOAD_STEP)
    parser.add_argument("--wait-nops", type=int, default=DEFAULT_WAIT_NOPS)
    parser.add_argument("--hit-threshold-ns", type=int,
                        default=DEFAULT_HIT_THRESHOLD_NS)
    parser.add_argument("--access", choices=["store", "load"], default="store")
    parser.add_argument("--inline", action="store_true",
                        help="Use inline access instead of noinline access.")
    parser.add_argument("--cc", default=os.environ.get("CC", "gcc"))
    parser.add_argument("--plot-only", action="store_true")
    parser.add_argument("--no-plot", action="store_true")
    args = parser.parse_args()

    if args.core < 0:
        parser.error("--core must be >= 0")
    if args.access_line < 0 or args.access_line >= 64:
        parser.error("--access-line must be in [0, 63]")
    if args.rounds < 1:
        parser.error("--rounds must be >= 1")
    if args.reload_step < 1:
        parser.error("--reload-step must be >= 1")
    if gcd(args.reload_step, 64) != 1:
        parser.error("--reload-step must be coprime with 64")
    if args.wait_nops < 0:
        parser.error("--wait-nops must be >= 0")
    if args.hit_threshold_ns < 1:
        parser.error("--hit-threshold-ns must be >= 1")
    return args


def gcd(a, b):
    while b:
        a, b = b, a % b
    return abs(a)


args = parse_args()
result_dir = os.path.join(BASE_DIR, "res", "single-shared-page-access")
plot_dir = os.path.join(BASE_DIR, "res", "barplots")
raw_dir = os.path.join(result_dir, "raw")


def pc_mode():
    return "inline" if args.inline else "noinline"


def micro_arch_name():
    return (
        f"{args.arch}-core{args.core}-single-shared-page"
        f"-line{args.access_line}-{args.access}-{pc_mode()}"
        f"-rounds{args.rounds}-reloadstep{args.reload_step}"
    )


def tsv_path():
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


def classify_position(position, avg_ns, probes):
    if position == args.access_line:
        return "accessed"
    if probes > 0 and avg_ns <= args.hit_threshold_ns:
        return "prefetched"
    return "cache_miss"


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
            position = int(fields[0])
            offset_bytes = int(fields[1])
            avg_ns = int(fields[2])
            probes = int(fields[3])
        except ValueError:
            print(f"Skipping non-numeric output row: {line}", file=sys.stderr)
            continue

        rows.append({
            "position": position,
            "offset_bytes": offset_bytes,
            "role": classify_position(position, avg_ns, probes),
            "avg_ns": avg_ns,
            "probes": probes,
        })
    return rows


def write_tsv(rows, path):
    with open(path, "w", newline="") as f:
        writer = csv.DictWriter(
            f,
            fieldnames=["position", "offset_bytes", "role", "avg_ns", "probes"],
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
                "position": int(row["position"]),
                "offset_bytes": int(row["offset_bytes"]),
                "role": row["role"],
                "avg_ns": int(row["avg_ns"]),
                "probes": int(row["probes"]),
            })
    return rows


def compile_test():
    compile_cmd = [
        args.cc,
        "-std=gnu11",
        "-O0",
        "-static",
        f"-DCPU_ID={args.core}",
        f"-DACCESS_LINE={args.access_line}",
        f"-DACCESS_IS_LOAD={1 if args.access == 'load' else 0}",
        f"-DUSE_NOINLINE_ACCESS={0 if args.inline else 1}",
        f"-DROUNDS={args.rounds}",
        f"-DRELOAD_STEP={args.reload_step}",
        f"-DWAIT_NOPS={args.wait_nops}",
        "-o",
        OUT,
        SRC,
        UTIL_SRC,
    ]
    return subprocess.run(compile_cmd)


def run_test():
    print("=" * 60)
    print(
        f"single shared-page access, arch={args.arch}, core={args.core}, "
        f"line={args.access_line}, access={args.access}, pc={pc_mode()}, "
        f"rounds={args.rounds}, reload_step={args.reload_step}, "
        f"wait_nops={args.wait_nops}"
    )

    compiled = compile_test()
    if compiled.returncode != 0:
        print("Compile failed")
        return []

    run = subprocess.run(
        ["taskset", "-c", str(args.core), OUT],
        capture_output=True,
        text=True,
    )
    if run.returncode != 0:
        print("Execution failed")
        if run.stdout:
            print(run.stdout)
        if run.stderr:
            print(run.stderr)
        return []

    with open(raw_path(), "w") as f:
        f.write(run.stdout)
    print(f"Saved raw output to {raw_path()}")

    rows = parse_output(run.stdout)
    if not rows:
        print("No results to save or plot")
        return []

    write_tsv(rows, tsv_path())
    print(f"Saved parsed results to {tsv_path()}")
    print_summary(rows)
    return rows


def print_summary(rows):
    print("Result summary:")
    for row in rows:
        if row["position"] in range(0, 16) or row["position"] == args.access_line:
            print(
                f"  line {row['position']:2d}: "
                f"{row['role']:10s} {row['avg_ns']:4d} ns"
            )


def plot_bar_chart(rows):
    try:
        import matplotlib.pyplot as plt
        from matplotlib.patches import Patch
    except ModuleNotFoundError as exc:
        print(f"Skipping bar plot: missing Python package '{exc.name}'.")
        return

    sorted_rows = sorted(rows, key=lambda row: row["position"])
    positions = [row["position"] for row in sorted_rows]
    values = [row["avg_ns"] for row in sorted_rows]
    colors = [
        ROLE_COLORS.get(row["role"], ROLE_COLORS["cache_miss"])
        for row in sorted_rows
    ]

    fig, ax = plt.subplots(1, 1, figsize=(14, 4))
    ax.bar(positions, values, color=colors, width=0.85,
           edgecolor="black", linewidth=0.25)
    ax.axhline(args.hit_threshold_ns, color="black",
               linestyle="--", linewidth=0.9)
    ax.axvline(args.access_line, color="#D55E00",
               linestyle=":", linewidth=1.0)

    ax.set_title("Single shared-page access reload map", loc="left", pad=4)
    ax.set_ylabel("Average reload ns")
    ax.set_xlabel("Cache-line index")
    ax.set_xlim(-1, 64)
    ax.set_ylim(0, max(300, max(values) * 1.05 if values else 300))
    ax.set_xticks(range(0, 64, 4))
    ax.grid(axis="y", alpha=0.25)

    legend_items = [
        Patch(facecolor=ROLE_COLORS["accessed"], edgecolor="black",
              label="accessed"),
        Patch(facecolor=ROLE_COLORS["prefetched"], edgecolor="black",
              label="prefetched"),
        Patch(facecolor=ROLE_COLORS["cache_miss"], edgecolor="black",
              label="cache_miss"),
    ]
    ax.legend(handles=legend_items, loc="upper right", frameon=False, ncol=3)

    fig.suptitle(
        f"{args.arch} core {args.core}, line={args.access_line}, "
        f"access={args.access}, pc={pc_mode()}, rounds={args.rounds}, "
        f"reload_step={args.reload_step}, threshold={args.hit_threshold_ns} ns",
        x=0.01,
        ha="left",
    )
    fig.tight_layout(rect=(0, 0, 1, 0.93))
    fig.savefig(plot_path(), dpi=300)
    plt.close(fig)
    print(f"Saved bar chart to {plot_path()}")


if __name__ == "__main__":
    ensure_dirs()
    if args.plot_only:
        if not os.path.exists(tsv_path()):
            print(f"Error: TSV result '{tsv_path()}' not found.")
            sys.exit(1)
        result_rows = read_tsv(tsv_path())
        print_summary(result_rows)
    else:
        result_rows = run_test()

    if result_rows and not args.no_plot:
        plot_bar_chart(result_rows)
