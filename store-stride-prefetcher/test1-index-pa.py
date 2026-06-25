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
SRC = os.path.join(BASE_DIR, "test1-index-pa.c")
UTIL_SRC = os.path.join(BASE_DIR, "until.c")
OUT = os.path.join(BASE_DIR, "bin", "test1-index-pa")

DEFAULT_STRIDE_LINES = 5
DEFAULT_TRAIN_STORES = None
DEFAULT_REPEAT = 5
DEFAULT_ROUNDS = 4000
DEFAULT_TRIGGER_MIN_LINE = 0
DEFAULT_TRIGGER_MAX_LINE = 128
DEFAULT_REF_TRIGGER_LINE = 61
DEFAULT_MAX_PA_BIT = 47
DEFAULT_BUDDY_MB = 64
DEFAULT_BUDDY_CHUNK_MB = 64
DEFAULT_ALIAS_MIN_M = 1
DEFAULT_ALIAS_MAX_M = 20

GREEN = "\033[32m"
RED = "\033[31m"
YELLOW = "\033[33m"
RESET = "\033[0m"

CASE_FIELDS = [
    "trigger_line",
    "trigger_offset",
    "trigger_va",
    "trigger_pa",
    "pa_xor_ref",
    "changed_pa_bits",
    "avg_ns",
    "prefetched",
    "note",
]

BIT_FIELDS = ["pa_bit", "isolated_case", "verdict"]
PA_BIT_FIELDS = [
    "pa_bit",
    "candidate_va",
    "candidate_pa",
    "pa_xor_ref",
    "avg_ns",
    "prefetched",
    "verdict",
    "source",
]
ALIAS_FIELDS = [
    "m_bits",
    "candidate_page_va",
    "candidate_page_pa",
    "page_pa_xor_ref",
    "avg_ns",
    "prefetched",
    "verdict",
    "source",
]


def parse_args():
    parser = argparse.ArgumentParser(
        description="Run and plot test1-index-pa.c PA-bit trigger-index sweep."
    )
    parser.add_argument("--arch", required=True, choices=arch_choices())
    parser.add_argument("--core", type=int, default=None,
                        help="Override CPU core. Default comes from --arch.")
    parser.add_argument("--stride", type=int, default=DEFAULT_STRIDE_LINES,
                        help="Stride in cache lines. Default: 5.")
    parser.add_argument("--train-stores", type=int,
                        default=DEFAULT_TRAIN_STORES,
                        help="Number of training stores. Default is arch store accesses.")
    parser.add_argument("--repeat", type=int, default=DEFAULT_REPEAT,
                        help="Repeat train+trigger sequence per round. Default: 5.")
    parser.add_argument("--rounds", type=int, default=DEFAULT_ROUNDS)
    parser.add_argument("--trigger-min-line", type=int,
                        default=DEFAULT_TRIGGER_MIN_LINE)
    parser.add_argument("--trigger-max-line", type=int,
                        default=DEFAULT_TRIGGER_MAX_LINE)
    parser.add_argument("--ref-trigger-line", type=int,
                        default=DEFAULT_REF_TRIGGER_LINE,
                        help="Reference trigger line used for PA xor. Default: 61.")
    parser.add_argument("--max-pa-bit", type=int, default=DEFAULT_MAX_PA_BIT)
    parser.add_argument("--mode", choices=["line-sweep", "pa-bits", "low-pa-alias"],
                        default="line-sweep",
                        help="line-sweep varies trigger lines; pa-bits searches exact PA single-bit buddies; low-pa-alias tests page pairs with matching low PFN bits.")
    parser.add_argument("--buddy-mb", type=int, default=DEFAULT_BUDDY_MB,
                        help="Candidate anonymous memory for --mode pa-bits. Default: 64 MB.")
    parser.add_argument("--buddy-chunk-mb", type=int,
                        default=DEFAULT_BUDDY_CHUNK_MB,
                        help="Chunk size for scanning --buddy-mb without mapping it all at once. Default: 64 MB.")
    parser.add_argument("--alias-min-m", type=int, default=DEFAULT_ALIAS_MIN_M,
                        help="First low-PFN-bit count for --mode low-pa-alias. Default: 1.")
    parser.add_argument("--alias-max-m", type=int, default=DEFAULT_ALIAS_MAX_M,
                        help="Last low-PFN-bit count for --mode low-pa-alias. Default: 20.")
    parser.add_argument("--threshold-ns", type=int, default=None,
                        help="Latency threshold for prefetched=yes. Default comes from --arch.")
    parser.add_argument("--cc", default=os.environ.get("CC", "gcc"))
    parser.add_argument("--output-prefix", default=None,
                        help="Output name prefix. Default is derived from config.")
    parser.add_argument("--plot-only", action="store_true",
                        help="Only read existing TSV files and draw plots.")
    parser.add_argument("--no-compile", action="store_true")
    parser.add_argument("--no-plot", action="store_true")
    parser.add_argument("--verbose", action="store_true",
                        help="Print raw C output after running.")
    args = parser.parse_args()

    apply_single_core_defaults(args)
    if args.train_stores is None:
        args.train_stores = ARCH_CONFIG[args.arch]["accesses"]["store"]
    apply_threshold_defaults(args)

    if args.core < 0:
        parser.error("--core must be >= 0")
    if args.stride < 1:
        parser.error("--stride must be >= 1")
    if args.train_stores < 1:
        parser.error("--train-stores must be >= 1")
    if args.repeat < 1:
        parser.error("--repeat must be >= 1")
    if args.rounds < 1:
        parser.error("--rounds must be >= 1")
    if args.trigger_min_line < 0:
        parser.error("--trigger-min-line must be >= 0")
    if args.trigger_max_line < args.trigger_min_line:
        parser.error("--trigger-max-line must be >= --trigger-min-line")
    if not (args.trigger_min_line <= args.ref_trigger_line <= args.trigger_max_line):
        parser.error("--ref-trigger-line must be inside trigger range")
    if args.max_pa_bit < 0 or args.max_pa_bit > 63:
        parser.error("--max-pa-bit must be in [0, 63]")
    if args.buddy_mb < 0:
        parser.error("--buddy-mb must be >= 0")
    if args.buddy_chunk_mb < 0:
        parser.error("--buddy-chunk-mb must be >= 0")
    if args.alias_min_m < 0:
        parser.error("--alias-min-m must be >= 0")
    if args.alias_max_m < args.alias_min_m:
        parser.error("--alias-max-m must be >= --alias-min-m")
    if args.alias_max_m > 51:
        parser.error("--alias-max-m must be <= 51")
    if args.threshold_ns < 1:
        parser.error("--threshold-ns must be >= 1")
    return args


args = parse_args()

RESULT_DIR = os.path.join(BASE_DIR, "res", "index-pa")
RAW_DIR = os.path.join(RESULT_DIR, "raw")
PLOT_DIR = os.path.join(RESULT_DIR, "plots")


def micro_arch_name():
    if args.output_prefix:
        return args.output_prefix
    base = (
        f"{args.arch}-core{args.core}-pa-index"
        f"-stride{args.stride}-train{args.train_stores}"
        f"-ref{args.ref_trigger_line}"
    )
    if args.mode == "pa-bits":
        return f"{base}-pa-bits-buddy{args.buddy_mb}MB"
    if args.mode == "low-pa-alias":
        return (
            f"{base}-low-pa-alias-M{args.alias_min_m}-{args.alias_max_m}"
            f"-buddy{args.buddy_mb}MB"
        )
    return f"{base}-range{args.trigger_min_line}-{args.trigger_max_line}"


def raw_path():
    return os.path.join(RAW_DIR, f"{micro_arch_name()}.txt")


def cases_tsv_path():
    return os.path.join(RESULT_DIR, f"{micro_arch_name()}-cases.tsv")


def bits_tsv_path():
    return os.path.join(RESULT_DIR, f"{micro_arch_name()}-bits.tsv")


def sweep_plot_path():
    return os.path.join(PLOT_DIR, f"{micro_arch_name()}-sweep.png")


def bits_plot_path():
    return os.path.join(PLOT_DIR, f"{micro_arch_name()}-bits.png")


def alias_tsv_path():
    return os.path.join(RESULT_DIR, f"{micro_arch_name()}-alias.tsv")


def alias_plot_path():
    return os.path.join(PLOT_DIR, f"{micro_arch_name()}-alias.png")


def ensure_dirs():
    for path in (OUT, raw_path(), cases_tsv_path(), bits_tsv_path(),
                 sweep_plot_path(), bits_plot_path(), alias_tsv_path(),
                 alias_plot_path()):
        directory = os.path.dirname(path)
        if directory:
            os.makedirs(directory, exist_ok=True)


def compile_test():
    cmd = [
        args.cc,
        "-std=gnu11",
        "-O0",
        "-static",
        f"-DSTRIDE_LINES={args.stride}",
        f"-DTRAIN_STORES={args.train_stores}",
        f"-DREPEAT={args.repeat}",
        f"-DROUNDS={args.rounds}",
        f"-DTRIGGER_MIN_LINE={args.trigger_min_line}",
        f"-DTRIGGER_MAX_LINE={args.trigger_max_line}",
        f"-DREF_TRIGGER_LINE={args.ref_trigger_line}",
        f"-DHIT_THRESHOLD_NS={args.threshold_ns}",
        f"-DCPU_ID={args.core}",
        f"-DARCH_NAME=\"{args.arch}\"",
        f"-DMAX_PA_BIT={args.max_pa_bit}",
        f"-DBUDDY_SCAN={1 if args.mode == 'pa-bits' else 0}",
        f"-DALIAS_SCAN={1 if args.mode == 'low-pa-alias' else 0}",
        f"-DBUDDY_PAGES={(args.buddy_mb * 1024 * 1024) // 4096}",
        f"-DBUDDY_CHUNK_PAGES={(args.buddy_chunk_mb * 1024 * 1024) // 4096}",
        f"-DALIAS_MIN_M={args.alias_min_m}",
        f"-DALIAS_MAX_M={args.alias_max_m}",
        "-o",
        OUT,
        SRC,
        UTIL_SRC,
    ]
    return subprocess.run(cmd).returncode


def run_binary():
    return subprocess.run(
        ["taskset", "-c", str(args.core), OUT],
        capture_output=True,
        text=True,
    )


def parse_c_output(output):
    cases = []
    bits = []
    aliases = []
    table = None

    for line in output.splitlines():
        stripped = line.strip()
        if not stripped or stripped.startswith("#"):
            continue

        fields = stripped.split()
        if fields == CASE_FIELDS:
            table = "cases"
            continue
        if fields == BIT_FIELDS:
            table = "bits"
            continue
        if fields == PA_BIT_FIELDS:
            table = "pa_bits"
            continue
        if fields == ALIAS_FIELDS:
            table = "aliases"
            continue

        if table == "cases":
            if len(fields) != len(CASE_FIELDS):
                print(f"Skipping unexpected case row: {line}", file=sys.stderr)
                continue
            row = dict(zip(CASE_FIELDS, fields))
            row["trigger_line"] = int(row["trigger_line"])
            row["trigger_offset"] = int(row["trigger_offset"])
            row["avg_ns"] = int(row["avg_ns"])
            cases.append(row)
        elif table == "bits":
            if len(fields) != len(BIT_FIELDS):
                print(f"Skipping unexpected bit row: {line}", file=sys.stderr)
                continue
            row = dict(zip(BIT_FIELDS, fields))
            row["pa_bit"] = int(row["pa_bit"])
            bits.append(row)
        elif table == "pa_bits":
            if len(fields) != len(PA_BIT_FIELDS):
                print(f"Skipping unexpected PA-bit row: {line}", file=sys.stderr)
                continue
            row = dict(zip(PA_BIT_FIELDS, fields))
            row["pa_bit"] = int(row["pa_bit"])
            row["avg_ns"] = int(row["avg_ns"])
            bits.append(row)
        elif table == "aliases":
            if len(fields) != len(ALIAS_FIELDS):
                print(f"Skipping unexpected alias row: {line}", file=sys.stderr)
                continue
            row = dict(zip(ALIAS_FIELDS, fields))
            row["m_bits"] = int(row["m_bits"])
            row["avg_ns"] = int(row["avg_ns"])
            aliases.append(row)

    return cases, bits, aliases


def write_tsv(rows, path, fields):
    with open(path, "w", newline="") as f:
        writer = csv.DictWriter(f, fieldnames=fields, delimiter="\t")
        writer.writeheader()
        writer.writerows(rows)


def read_tsv(path, fields):
    rows = []
    with open(path, newline="") as f:
        reader = csv.DictReader(f, delimiter="\t")
        for row in reader:
            if fields == CASE_FIELDS:
                row["trigger_line"] = int(row["trigger_line"])
                row["trigger_offset"] = int(row["trigger_offset"])
                row["avg_ns"] = int(row["avg_ns"])
            elif fields == ALIAS_FIELDS:
                row["m_bits"] = int(row["m_bits"])
                row["avg_ns"] = int(row["avg_ns"])
            else:
                row["pa_bit"] = int(row["pa_bit"])
                if "avg_ns" in row:
                    row["avg_ns"] = int(row["avg_ns"])
            rows.append(row)
    return rows


def verdict_color(verdict):
    if verdict == "not_participating":
        return GREEN
    if verdict == "participating":
        return RED
    return YELLOW


def print_summary(cases, bits):
    yes_cases = [row for row in cases if row["prefetched"] == "yes"]
    no_cases = [row for row in cases if row["prefetched"] == "no"]
    participating = [row["pa_bit"] for row in bits
                     if row["verdict"] == "participating"]
    not_participating = [row["pa_bit"] for row in bits
                         if row["verdict"] == "not_participating"]
    insufficient = [row["pa_bit"] for row in bits
                    if row["verdict"] == "insufficient_case"]

    if cases:
        print(
            f"cases: prefetched={len(yes_cases)} non_prefetched={len(no_cases)} "
            f"threshold={args.threshold_ns} ns"
        )
    else:
        found = len(bits) - len(insufficient)
        print(
            f"pa-bit candidates: found={found} insufficient={len(insufficient)} "
            f"threshold={args.threshold_ns} ns"
        )
    print(
        f"single-bit verdicts: "
        f"{GREEN}not_participating={not_participating}{RESET} "
        f"{RED}participating={participating}{RESET} "
        f"{YELLOW}insufficient={len(insufficient)} bits{RESET}"
    )


def print_alias_summary(aliases):
    found = [row for row in aliases if row["verdict"] != "insufficient_case"]
    triggered = [row for row in aliases if row["verdict"] == "alias_triggered"]
    no_alias = [row for row in aliases if row["verdict"] == "no_alias"]
    insufficient = [row for row in aliases
                    if row["verdict"] == "insufficient_case"]

    print(
        f"alias candidates: found={len(found)} triggered={len(triggered)} "
        f"no_alias={len(no_alias)} insufficient={len(insufficient)} "
        f"threshold={args.threshold_ns} ns"
    )
    print(
        f"alias_triggered M={[row['m_bits'] for row in triggered]} "
        f"no_alias M={[row['m_bits'] for row in no_alias]}"
    )


def require_matplotlib():
    try:
        import matplotlib.pyplot as plt
        from matplotlib.patches import Patch
    except ModuleNotFoundError as exc:
        print(f"Skipping plots: missing Python package '{exc.name}'.")
        print("Install plotting dependencies with:")
        print("  sudo apt install python3-matplotlib")
        print("or:")
        print("  python3 -m pip install matplotlib")
        return None, None
    return plt, Patch


def plot_sweep(cases):
    plt, Patch = require_matplotlib()
    if plt is None:
        return

    rows = sorted(cases, key=lambda row: row["trigger_line"])
    x = [row["trigger_line"] for row in rows]
    y = [row["avg_ns"] for row in rows]
    colors = []

    for row in rows:
        if row["note"] == "direct_probe_line":
            colors.append("#CC79A7")
        elif row["prefetched"] == "yes":
            colors.append("#0072B2")
        elif row["note"] == "training_line":
            colors.append("#D55E00")
        else:
            colors.append("#BDBDBD")

    width = min(max((args.trigger_max_line - args.trigger_min_line + 1) / 8, 10), 22)
    fig, ax = plt.subplots(1, 1, figsize=(width, 4.8))

    ax.bar(x, y, color=colors, width=0.85, edgecolor="black", linewidth=0.2)
    ax.axhline(args.threshold_ns, color="black", linestyle="--", linewidth=0.9)
    ax.axvline(args.ref_trigger_line, color="#009E73",
               linestyle=":", linewidth=1.1)
    ax.axvline(args.train_stores * args.stride, color="#CC79A7",
               linestyle=":", linewidth=1.1)

    ax.set_title("Trigger target sweep", loc="left")
    ax.set_xlabel("Trigger cache-line index")
    ax.set_ylabel("Average probe latency ns")
    ax.set_xlim(args.trigger_min_line - 1, args.trigger_max_line + 1)
    ax.set_ylim(0, max(300, max(y) * 1.08 if y else 300))
    ax.grid(axis="y", alpha=0.25)

    tick_step = max(1, (args.trigger_max_line - args.trigger_min_line + 1) // 16)
    ax.set_xticks(range(args.trigger_min_line, args.trigger_max_line + 1, tick_step))

    legend_items = [
        Patch(facecolor="#0072B2", edgecolor="black", label="prefetched"),
        Patch(facecolor="#BDBDBD", edgecolor="black", label="not prefetched"),
        Patch(facecolor="#D55E00", edgecolor="black", label="training line"),
        Patch(facecolor="#CC79A7", edgecolor="black", label="direct probe line"),
    ]
    ax.legend(handles=legend_items, loc="upper right", frameon=False, ncol=4)

    fig.suptitle(
        f"{args.arch} core {args.core}, stride={args.stride}, "
        f"train={args.train_stores}, ref={args.ref_trigger_line}, "
        f"threshold={args.threshold_ns} ns",
        x=0.01,
        ha="left",
    )
    fig.tight_layout(rect=(0, 0, 1, 0.92))
    fig.savefig(sweep_plot_path(), dpi=300)
    plt.close(fig)
    print(f"Saved sweep plot to {sweep_plot_path()}")


def plot_bits(bits):
    plt, Patch = require_matplotlib()
    if plt is None:
        return

    rows = sorted(bits, key=lambda row: row["pa_bit"])
    x = [row["pa_bit"] for row in rows]
    has_latency = bool(rows) and "avg_ns" in rows[0]
    if has_latency:
        values = [
            row["avg_ns"] if row["verdict"] != "insufficient_case" else 0
            for row in rows
        ]
    else:
        values = [
            1 if row["verdict"] != "insufficient_case" else 0
            for row in rows
        ]
    colors = []
    for row in rows:
        if row["verdict"] == "not_participating":
            colors.append("#009E73")
        elif row["verdict"] == "participating":
            colors.append("#D55E00")
        else:
            colors.append("#BDBDBD")

    fig, ax = plt.subplots(1, 1, figsize=(14, 3.8))
    ax.bar(x, values, color=colors, width=0.85, edgecolor="black", linewidth=0.2)
    ax.set_title("Single-bit PA verdicts", loc="left")
    ax.set_xlabel("PA bit")
    if has_latency:
        ax.axhline(args.threshold_ns, color="black", linestyle="--", linewidth=0.9)
        ax.set_ylabel("Average probe latency ns")
        ax.set_ylim(0, max(300, max(values) * 1.08 if values else 300))
    else:
        ax.set_yticks([0, 1])
        ax.set_yticklabels(["insufficient", "isolated"])
        ax.set_ylim(0, 1.25)
    ax.set_xlim(-1, args.max_pa_bit + 1)
    ax.grid(axis="y", alpha=0.25)

    tick_step = 1 if args.max_pa_bit <= 32 else 2
    ax.set_xticks(range(0, args.max_pa_bit + 1, tick_step))

    legend_items = [
        Patch(facecolor="#009E73", edgecolor="black", label="not participating"),
        Patch(facecolor="#D55E00", edgecolor="black", label="participating"),
        Patch(facecolor="#BDBDBD", edgecolor="black", label="insufficient case"),
    ]
    ax.legend(handles=legend_items, loc="upper right", frameon=False, ncol=3)

    fig.suptitle(
        f"{args.arch} core {args.core}, ref trigger line {args.ref_trigger_line}",
        x=0.01,
        ha="left",
    )
    fig.tight_layout(rect=(0, 0, 1, 0.9))
    fig.savefig(bits_plot_path(), dpi=300)
    plt.close(fig)
    print(f"Saved bit plot to {bits_plot_path()}")


def plot_aliases(aliases):
    plt, Patch = require_matplotlib()
    if plt is None:
        return

    rows = sorted(aliases, key=lambda row: row["m_bits"])
    x = [row["m_bits"] for row in rows]
    y = [row["avg_ns"] for row in rows]
    colors = []

    for row in rows:
        if row["verdict"] == "alias_triggered":
            colors.append("#009E73")
        elif row["verdict"] == "no_alias":
            colors.append("#D55E00")
        else:
            colors.append("#BDBDBD")

    fig, ax = plt.subplots(1, 1, figsize=(12, 4.2))
    ax.bar(x, y, color=colors, width=0.85, edgecolor="black", linewidth=0.2)
    ax.axhline(args.threshold_ns, color="black", linestyle="--", linewidth=0.9)
    ax.set_title("Low-PA alias scan", loc="left")
    ax.set_xlabel("M matching low PFN bits: PA[12..12+M-1]")
    ax.set_ylabel("Average probe latency ns")
    ax.set_xlim(min(x) - 1 if x else args.alias_min_m - 1,
                max(x) + 1 if x else args.alias_max_m + 1)
    ax.set_ylim(0, max(300, max(y) * 1.08 if y else 300))
    ax.grid(axis="y", alpha=0.25)
    ax.set_xticks(range(args.alias_min_m, args.alias_max_m + 1))

    legend_items = [
        Patch(facecolor="#009E73", edgecolor="black", label="alias triggered"),
        Patch(facecolor="#D55E00", edgecolor="black", label="no alias"),
        Patch(facecolor="#BDBDBD", edgecolor="black", label="insufficient"),
    ]
    ax.legend(handles=legend_items, loc="upper right", frameon=False, ncol=3)
    fig.suptitle(
        f"{args.arch} core {args.core}, ref trigger line {args.ref_trigger_line}, "
        f"buddy={args.buddy_mb} MB",
        x=0.01,
        ha="left",
    )
    fig.tight_layout(rect=(0, 0, 1, 0.9))
    fig.savefig(alias_plot_path(), dpi=300)
    plt.close(fig)
    print(f"Saved alias plot to {alias_plot_path()}")


def main():
    ensure_dirs()

    if args.plot_only:
        bit_fields = PA_BIT_FIELDS if args.mode == "pa-bits" else BIT_FIELDS
        if args.mode == "line-sweep":
            if not os.path.exists(cases_tsv_path()):
                print("Missing case TSV for --plot-only:", file=sys.stderr)
                print(f"  {cases_tsv_path()}", file=sys.stderr)
                return 1
            cases = read_tsv(cases_tsv_path(), CASE_FIELDS)
            aliases = []
        elif args.mode == "low-pa-alias":
            cases = []
            bits = []
            if not os.path.exists(alias_tsv_path()):
                print("Missing alias TSV for --plot-only:", file=sys.stderr)
                print(f"  {alias_tsv_path()}", file=sys.stderr)
                return 1
            aliases = read_tsv(alias_tsv_path(), ALIAS_FIELDS)
        else:
            cases = []
            aliases = []
        if args.mode != "low-pa-alias" and not os.path.exists(bits_tsv_path()):
            print("Missing bit TSV for --plot-only:", file=sys.stderr)
            print(f"  {bits_tsv_path()}", file=sys.stderr)
            return 1
        if args.mode != "low-pa-alias":
            bits = read_tsv(bits_tsv_path(), bit_fields)
    else:
        print(
            f"arch={args.arch}, core={args.core}, stride={args.stride}, "
            f"train_stores={args.train_stores}, repeat={args.repeat}, "
            f"rounds={args.rounds}, mode={args.mode}, "
            f"trigger_range={args.trigger_min_line}..{args.trigger_max_line}, "
            f"ref={args.ref_trigger_line}, threshold={args.threshold_ns} ns, "
            f"buddy_mb={args.buddy_mb}, buddy_chunk_mb={args.buddy_chunk_mb}, "
            f"alias_m={args.alias_min_m}..{args.alias_max_m}"
        )

        if not args.no_compile and compile_test() != 0:
            print("Compile failed", file=sys.stderr)
            return 1

        run = run_binary()
        if args.verbose and run.stdout:
            print(run.stdout, end="")
        if run.stderr:
            print(run.stderr, end="", file=sys.stderr)
        if run.returncode != 0:
            if run.stdout and not args.verbose:
                print(run.stdout, end="", file=sys.stderr)
            print("Execution failed", file=sys.stderr)
            return run.returncode

        with open(raw_path(), "w") as f:
            f.write(run.stdout)

        cases, bits, aliases = parse_c_output(run.stdout)
        if args.mode == "line-sweep" and (not cases or not bits):
            print("No result rows parsed", file=sys.stderr)
            return 1
        if args.mode == "pa-bits" and not bits:
            print("No PA-bit result rows parsed", file=sys.stderr)
            return 1
        if args.mode == "low-pa-alias" and not aliases:
            print("No alias result rows parsed", file=sys.stderr)
            return 1

        if cases:
            write_tsv(cases, cases_tsv_path(), CASE_FIELDS)
        bit_fields = PA_BIT_FIELDS if args.mode == "pa-bits" else BIT_FIELDS
        if bits:
            write_tsv(bits, bits_tsv_path(), bit_fields)
        if aliases:
            write_tsv(aliases, alias_tsv_path(), ALIAS_FIELDS)
        print(f"Saved raw output to {raw_path()}")
        if cases:
            print(f"Saved case TSV to {cases_tsv_path()}")
        if bits:
            print(f"Saved bit TSV to {bits_tsv_path()}")
        if aliases:
            print(f"Saved alias TSV to {alias_tsv_path()}")

    if args.mode == "low-pa-alias":
        print_alias_summary(aliases)
    else:
        print_summary(cases, bits)

    if not args.no_plot:
        if aliases:
            plot_aliases(aliases)
        elif cases:
            plot_sweep(cases)
        if bits:
            plot_bits(bits)

    return 0


if __name__ == "__main__":
    sys.exit(main())
