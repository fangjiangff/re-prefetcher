import argparse
import math
import os
import subprocess
import sys

import matplotlib.pyplot as plt


SRC = "test-npc-load-streams.c"
OUT = "bin/test-npc-load-streams"
DEFAULT_N_PCS = 12
DEFAULT_ROUNDS = 200
DEFAULT_CORE = 2
DEFAULT_BASE_PC = "0x500200140"
DEFAULT_BUFFER = "0x610000000"
DEFAULT_OUTPUT = "res/npc-load-streams.tsv"
DEFAULT_PLOT = "res/heatmaps/npc-load-streams.png"


def compile_test():
    os.makedirs(os.path.dirname(OUT), exist_ok=True)
    compile_cmd = [
        "gcc",
        "-std=gnu11",
        "-O0",
        "-static",
        "-o",
        OUT,
        SRC,
    ]
    return subprocess.run(compile_cmd)


def run_test(args):
    run_cmd = [
        "taskset",
        "-c",
        str(args.core),
        "./" + OUT,
        str(args.n_pcs),
        str(args.rounds),
        args.base_pc,
        args.buffer,
    ]
    return subprocess.run(run_cmd, capture_output=True, text=True)


def save_text(path, text):
    if not path:
        return
    parent = os.path.dirname(path)
    if parent:
        os.makedirs(parent, exist_ok=True)
    with open(path, "w") as f:
        f.write(text)


def parse_table(text):
    rows = []
    stream_labels = []

    for line in text.splitlines():
        if not line or line.startswith("#"):
            continue

        parts = line.rstrip("\n").split("\t")
        if parts[0] == "m_streams":
            stream_labels = parts[1:]
            continue

        if parts[0].isdigit():
            m_streams = int(parts[0])
            values = []
            for item in parts[1:]:
                if item == "":
                    values.append(math.nan)
                else:
                    values.append(float(item))
            rows.append((m_streams, values))

    if not rows:
        raise ValueError("no result rows found in test output")

    width = max(len(values) for _, values in rows)
    if not stream_labels:
        stream_labels = [f"stream{i}" for i in range(width)]

    matrix = []
    y_labels = []
    for m_streams, values in rows:
        padded = values + [math.nan] * (width - len(values))
        matrix.append(padded)
        y_labels.append(str(m_streams))

    return stream_labels[:width], y_labels, matrix


def plot_heatmap(text, output_path, title):
    stream_labels, y_labels, matrix = parse_table(text)

    os.makedirs(os.path.dirname(output_path), exist_ok=True)
    fig_width = max(7, 0.75 * len(stream_labels) + 2)
    fig_height = max(5, 0.45 * len(y_labels) + 2)
    fig, ax = plt.subplots(figsize=(fig_width, fig_height))

    image = ax.imshow(matrix, cmap="viridis_r", aspect="auto", vmin=70, vmax=240)
    ax.set_title(title)
    ax.set_xlabel("Stream")
    ax.set_ylabel("m_streams")
    ax.set_xticks(range(len(stream_labels)))
    ax.set_xticklabels(stream_labels, rotation=45, ha="right")
    ax.set_yticks(range(len(y_labels)))
    ax.set_yticklabels(y_labels)

    for y, row in enumerate(matrix):
        for x, value in enumerate(row):
            if math.isnan(value):
                continue
            ax.text(x, y, f"{int(value)}", ha="center", va="center", fontsize=8)

    cbar = fig.colorbar(image, ax=ax)
    cbar.set_label("Latency (ns)")
    fig.tight_layout()
    fig.savefig(output_path, dpi=300)
    plt.close(fig)


def main():
    parser = argparse.ArgumentParser(
        description="Compile, run, and plot test-npc-load-streams.c."
    )
    parser.add_argument("--n-pcs", type=int, default=DEFAULT_N_PCS)
    parser.add_argument("--rounds", type=int, default=DEFAULT_ROUNDS)
    parser.add_argument("--core", type=int, default=DEFAULT_CORE)
    parser.add_argument("--base-pc", default=DEFAULT_BASE_PC)
    parser.add_argument("--buffer", default=DEFAULT_BUFFER)
    parser.add_argument("--output", default=DEFAULT_OUTPUT)
    parser.add_argument("--plot", default=DEFAULT_PLOT)
    parser.add_argument("--no-compile", action="store_true")
    parser.add_argument("--plot-only", action="store_true")
    args = parser.parse_args()

    if args.plot_only:
        if not args.output:
            print("--plot-only requires --output", file=sys.stderr)
            return 1
        with open(args.output) as f:
            output = f.read()
    else:
        if not args.no_compile:
            res = compile_test()
            if res.returncode != 0:
                print("Compile failed", file=sys.stderr)
                return res.returncode

        run = run_test(args)
        if run.stdout:
            print(run.stdout, end="")
        if run.stderr:
            print(run.stderr, end="", file=sys.stderr)
        if run.returncode != 0:
            print("Execution failed", file=sys.stderr)
            return run.returncode

        output = run.stdout
        save_text(args.output, output)
        if args.output:
            print(f"Saved result to {args.output}")

    if args.plot:
        title = f"NPC load streams: n_pcs={args.n_pcs}, rounds={args.rounds}"
        plot_heatmap(output, args.plot, title)
        print(f"Saved plot to {args.plot}")

    return 0


if __name__ == "__main__":
    sys.exit(main())
