import argparse
import csv
import os
import subprocess
import sys

ROOT_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
if ROOT_DIR not in sys.path:
    sys.path.insert(0, ROOT_DIR)

from cross_test_config import ARCH_CONFIG, arch_choices


BASE_DIR = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.join(BASE_DIR, "test0-pc.c")
UTIL_SRC = os.path.join(ROOT_DIR, "until.c")
OUT = os.path.join(ROOT_DIR, "bin", "test0-pc")

RESULT_DIR = os.path.join(ROOT_DIR, "res", "store-stride")
RAW_DIR = os.path.join(RESULT_DIR, "raw")
DUMP_DIR = os.path.join(RESULT_DIR, "dump")
PLOT_DIR = os.path.join(ROOT_DIR, "res", "barplots")

ROLE_COLORS = {
    "accessed": "#D55E00",
    "prefetched": "#0072B2",
    "cache_miss": "#BDBDBD",
}
TRAIN_STORE_STEPS = (0, 1, 2, 3, 4)


def parse_args():
    parser = argparse.ArgumentParser(
        description="Run test0-pc with five controlled training store PCs."
    )
    parser.add_argument("--arch", choices=arch_choices(), default="A76")
    parser.add_argument("--core", type=int, default=None)
    parser.add_argument("--stride", type=int, default=5,
                        help="Stride in cache lines. Default: 5")
    parser.add_argument("--train-step", type=int, default=3)
    parser.add_argument("--rounds", type=int, default=40000)
    parser.add_argument("--probe-positions", type=int, default=100)
    parser.add_argument("--dummy-buffer-pages", type=int, default=10)
    parser.add_argument("--context-switch", action="store_true")
    parser.add_argument("--context-switch-yields", type=int, default=1)
    parser.add_argument("--train-pc0", default="0x783709b0120")
    parser.add_argument("--train-pc1", default="0x2d650271c2a4")
    parser.add_argument("--train-pc2", default="0x646f3e8ac548")
    parser.add_argument("--train-pc3", default="0x3a74fdac8a90")
    parser.add_argument("--train-pc4", default="0x53e91b4d67c0")
    parser.add_argument("--cc", default=os.environ.get("CC", "gcc"))
    parser.add_argument("--objdump", default=os.environ.get("OBJDUMP", "objdump"))
    parser.add_argument("--no-compile", action="store_true")
    parser.add_argument("--no-dump", action="store_true")
    parser.add_argument("--plot-only", action="store_true",
                        help="Only read existing TSV results and generate the bar plot.")
    parser.add_argument("--no-plot", action="store_true",
                        help="Do not generate the bar plot.")
    parser.add_argument("--hit-threshold-ns", type=int, default=None,
                        help="Cache hit threshold for prefetched classification. "
                             "Default is selected from --arch.")
    args = parser.parse_args()

    if args.core is None:
        args.core = ARCH_CONFIG[args.arch]["core"]
    if args.core < 0:
        parser.error("--core must be >= 0")
    if args.stride < 1:
        parser.error("--stride must be >= 1")
    if args.train_step < 1:
        parser.error("--train-step must be >= 1")
    if args.rounds < 0:
        parser.error("--rounds must be >= 0")
    if args.probe_positions < 1:
        parser.error("--probe-positions must be >= 1")
    if args.dummy_buffer_pages < 1:
        parser.error("--dummy-buffer-pages must be >= 1")
    if args.context_switch_yields < 1:
        parser.error("--context-switch-yields must be >= 1")
    if args.hit_threshold_ns is not None and args.hit_threshold_ns < 1:
        parser.error("--hit-threshold-ns must be >= 1")
    if args.train_step * args.stride >= args.probe_positions:
        parser.error("predicted position train_step * stride must be inside probe positions")

    for name in ("train_pc0", "train_pc1", "train_pc2", "train_pc3", "train_pc4"):
        value = int(getattr(args, name), 0)
        if value < 4 or value % 4 != 0:
            parser.error(f"--{name.replace('_', '-')} must be 4-byte aligned and >= 4")
        setattr(args, name, value)

    return args


def ensure_dirs():
    os.makedirs(os.path.dirname(OUT), exist_ok=True)
    os.makedirs(RESULT_DIR, exist_ok=True)
    os.makedirs(RAW_DIR, exist_ok=True)
    os.makedirs(DUMP_DIR, exist_ok=True)
    os.makedirs(PLOT_DIR, exist_ok=True)


def arch_cflags_for(arch):
    if arch in {"x86", "Zen4"}:
        return []
    return ["-march=armv8.5-a+predres"]


def run_name(args):
    switch_suffix = (
        f"-ctxswitch{args.context_switch_yields}"
        if args.context_switch else ""
    )
    return (
        f"{args.arch}-core{args.core}-stride{args.stride}"
        f"-train{args.train_step}-store-pc{switch_suffix}"
    )


def raw_path(args):
    return os.path.join(RAW_DIR, run_name(args) + ".txt")


def tsv_path(args):
    return os.path.join(RESULT_DIR, run_name(args) + ".tsv")


def dump_path(args):
    return os.path.join(DUMP_DIR, run_name(args) + ".dump")


def plot_path(args):
    return os.path.join(PLOT_DIR, run_name(args) + ".png")


def threshold_ns(args):
    if args.hit_threshold_ns is not None:
        return args.hit_threshold_ns
    return ARCH_CONFIG[args.arch]["threshold_ns"]


def predicted_position(args):
    return args.train_step * args.stride


def accessed_offsets(args):
    stride_bytes = args.stride * 64
    return {step * stride_bytes for step in TRAIN_STORE_STEPS}


def classify_position(args, offset_bytes, avg_ns, probes):
    if offset_bytes in accessed_offsets(args):
        return "accessed"
    if probes > 0 and avg_ns <= threshold_ns(args):
        return "prefetched"
    return "cache_miss"


def parse_output(args, output):
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
            "role": classify_position(args, offset_bytes, avg_ns, probes),
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


def compile_test(args):
    cmd = [
        args.cc,
        "-std=gnu11",
        "-O0",
        "-static",
         "-march=armv8.5-a+predres",
        f"-DSTRIDE_BYTES={args.stride * 64}",
        f"-DTRAIN_STEP={args.train_step}",
        f"-DROUNDS={args.rounds}",
        f"-DPROBE_POSITIONS={args.probe_positions}",
        f"-DDUMMY_BUFFER_PAGES={args.dummy_buffer_pages}",
        f"-DCONTEXT_SWITCH_BEFORE_TRIGGER={1 if args.context_switch else 0}",
        f"-DCONTEXT_SWITCH_YIELDS={args.context_switch_yields}",
        f"-DTRAIN_STORE_PC0={args.train_pc0:#x}ull",
        f"-DTRAIN_STORE_PC1={args.train_pc1:#x}ull",
        f"-DTRAIN_STORE_PC2={args.train_pc2:#x}ull",
        f"-DTRAIN_STORE_PC3={args.train_pc3:#x}ull",
        f"-DTRAIN_STORE_PC4={args.train_pc4:#x}ull",
        "-o",
        OUT,
        SRC,
        UTIL_SRC,
    ]
    cmd[1:1] = arch_cflags_for(args.arch)
    print(" ".join(cmd))
    return subprocess.run(cmd).returncode


def write_dump(args):
    if args.no_dump:
        return

    path = dump_path(args)
    run = subprocess.run([args.objdump, "-d", OUT],
                         capture_output=True, text=True)
    if run.returncode != 0:
        print("failed to generate dump", file=sys.stderr)
        if run.stderr:
            print(run.stderr, file=sys.stderr)
        return

    with open(path, "w") as f:
        f.write(run.stdout)
    print(f"Saved dump to {path}")


def run_binary(args):
    return subprocess.run(["taskset", "-c", str(args.core), OUT],
                          capture_output=True, text=True)


def plot_bar_chart(args, rows):
    try:
        import matplotlib.pyplot as plt
        from matplotlib.patches import Patch
    except ModuleNotFoundError as exc:
        print(f"Skipping bar plot: missing Python package '{exc.name}'.")
        print("Install plotting dependencies with:")
        print("  sudo apt install python3-matplotlib")
        print("or, for the current Python environment:")
        print("  python3 -m pip install matplotlib")
        return

    sorted_rows = sorted(rows, key=lambda row: row["position"])
    positions = [row["position"] for row in sorted_rows]
    values = [row["avg_ns"] for row in sorted_rows]
    colors = [
        ROLE_COLORS.get(row["role"], ROLE_COLORS["cache_miss"])
        for row in sorted_rows
    ]

    width = min(max(args.probe_positions / 5, 10), 18)
    fig, ax = plt.subplots(1, 1, figsize=(width, 4))

    ax.bar(positions, values, color=colors, width=0.85,
           edgecolor="black", linewidth=0.25)
    ax.axhline(threshold_ns(args), color="black",
               linestyle="--", linewidth=0.9)
    ax.axvline(predicted_position(args), color="#0072B2",
               linestyle=":", linewidth=1.0)

    ax.set_title(
        f"controlled-PC store stride, train-step={args.train_step}",
        loc="left",
        pad=4,
    )
    ax.set_ylabel("Average reload ns")
    ax.set_xlabel("Probe cache-line index")
    ax.set_ylim(0, max(300, max(values) * 1.05 if values else 300))
    ax.set_xlim(-1, max(positions) + 1 if positions else args.probe_positions)
    ax.grid(axis="y", alpha=0.25)

    tick_step = max(1, args.probe_positions // 16)
    ax.set_xticks(range(0, args.probe_positions, tick_step))

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
        f"{args.arch} core {args.core}, stride={args.stride}, "
        f"predicted_position={predicted_position(args)}, "
        f"threshold={threshold_ns(args)} ns",
        x=0.01,
        ha="left",
    )
    fig.tight_layout(rect=(0, 0, 1, 0.94))
    path = plot_path(args)
    fig.savefig(path, dpi=300)
    plt.close(fig)
    print(f"Saved bar chart to {path}")


def main():
    args = parse_args()
    ensure_dirs()

    if args.plot_only:
        path = tsv_path(args)
        if not os.path.exists(path):
            print(f"Error: TSV result '{path}' not found.", file=sys.stderr)
            return 1
        rows = read_tsv(path)
        if not args.no_plot:
            plot_bar_chart(args, rows)
        return 0

    if not args.no_compile and compile_test(args) != 0:
        return 1

    write_dump(args)
    run = run_binary(args)

    if run.stdout:
        print(run.stdout, end="")
    if run.stderr:
        print(run.stderr, file=sys.stderr, end="")

    if run.returncode != 0:
        print(f"Execution failed with return code {run.returncode}",
              file=sys.stderr)
        return run.returncode

    with open(raw_path(args), "w") as f:
        f.write(run.stdout)
    print(f"Saved raw output to {raw_path(args)}")

    rows = parse_output(args, run.stdout)
    if rows:
        write_tsv(rows, tsv_path(args))
        print(f"Saved TSV result to {tsv_path(args)}")
        if not args.no_plot:
            plot_bar_chart(args, rows)

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
