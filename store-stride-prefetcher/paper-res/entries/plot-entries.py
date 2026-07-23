#!/usr/bin/env python3
"""Plot L2D hardware-prefetch values versus the number of other pages."""

from __future__ import annotations

import argparse
from pathlib import Path

import matplotlib.pyplot as plt
from matplotlib.ticker import MultipleLocator


plt.rcParams.update(
    {
        "font.family": "serif",
        "font.serif": [
            "Times New Roman",
            "Liberation Serif",
            "Nimbus Roman",
            "DejaVu Serif",
        ],
        "font.size": 13,
        "axes.labelsize": 15,
        "legend.fontsize": 12,
        "xtick.labelsize": 11,
        "ytick.labelsize": 12,
        "axes.linewidth": 0.8,
        "pdf.fonttype": 42,
        "ps.fonttype": 42,
    }
)


SCRIPT_DIR = Path(__file__).resolve().parent
DEFAULT_INPUT = SCRIPT_DIR / "entry-hwprf.txt"
DEFAULT_OUTPUT = SCRIPT_DIR / "entry-hwprf-plot.png"


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "-i",
        "--input",
        type=Path,
        default=DEFAULT_INPUT,
        help="input table (default: %(default)s)",
    )
    parser.add_argument(
        "-o",
        "--output",
        type=Path,
        default=DEFAULT_OUTPUT,
        help=(
            "output base path; matching .png and .eps files are written "
            "(default: %(default)s)"
        ),
    )
    parser.add_argument("--dpi", type=int, default=300, help="output DPI")
    parser.add_argument(
        "--show", action="store_true", help="also display the plot interactively"
    )
    return parser.parse_args()


def load_trigger_data(path: Path) -> tuple[list[int], list[float]]:
    if not path.is_file():
        raise FileNotFoundError(f"input file does not exist: {path}")

    pages: list[int] = []
    trigger_values: list[float] = []
    with path.open(encoding="utf-8") as input_file:
        header = input_file.readline().split()
        required = {"other_pages", "trigger"}
        missing = required.difference(header)
        if missing:
            raise ValueError(f"{path}: missing columns: {', '.join(sorted(missing))}")

        page_index = header.index("other_pages")
        trigger_index = header.index("trigger")
        for line_number, line in enumerate(input_file, start=2):
            fields = line.split()
            if not fields:
                continue
            if len(fields) != len(header):
                raise ValueError(
                    f"{path}:{line_number}: expected {len(header)} columns"
                )
            try:
                pages.append(int(fields[page_index]))
                trigger_values.append(float(fields[trigger_index]))
            except ValueError as error:
                raise ValueError(
                    f"{path}:{line_number}: invalid numeric value"
                ) from error

    order = sorted(range(len(pages)), key=pages.__getitem__)
    pages = [pages[index] for index in order]
    trigger_values = [trigger_values[index] for index in order]
    if pages != list(range(33)):
        raise ValueError(f"{path}: other_pages must contain every value from 0 to 32")
    return pages, trigger_values


def draw_plot(pages: list[int], trigger_values: list[float]) -> plt.Figure:
    figure, axis = plt.subplots(figsize=(6.8, 2.6))
    axis.plot(
        pages,
        trigger_values,
        color="#2166ac",
        linestyle="-",
        linewidth=1.7,
        marker="o",
        markersize=4.8,
        markerfacecolor="#2166ac",
        markeredgecolor="#174a7e",
        markeredgewidth=0.8,
        zorder=3,
    )

    axis.set_xlabel("Number of competitor pages")
    axis.set_ylabel("Avergae Prefetches")
    axis.set_xlim(0, 32)
    axis.set_ylim(bottom=-0.5)
    axis.set_xticks(range(0, 33, 2))
    axis.yaxis.set_major_locator(MultipleLocator(1))
    axis.tick_params(axis="both", which="both", direction="in", top=True, right=True)
    axis.grid(axis="both", linestyle="--", linewidth=0.45, color="0.82")
    axis.set_axisbelow(True)
    figure.tight_layout(pad=0.5)
    return figure


def main() -> None:
    args = parse_args()
    pages, trigger_values = load_trigger_data(args.input)
    figure = draw_plot(pages, trigger_values)

    output_base = (
        args.output.with_suffix("")
        if args.output.suffix.lower() in {".png", ".eps"}
        else args.output
    )
    png_output = output_base.with_suffix(".png")
    eps_output = output_base.with_suffix(".eps")
    output_base.parent.mkdir(parents=True, exist_ok=True)
    figure.savefig(png_output, dpi=args.dpi, bbox_inches="tight")
    figure.savefig(eps_output, format="eps", bbox_inches="tight")
    print(f"Saved {png_output}")
    print(f"Saved {eps_output}")

    if args.show:
        plt.show()
    else:
        plt.close(figure)


if __name__ == "__main__":
    main()
