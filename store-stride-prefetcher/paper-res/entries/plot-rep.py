#!/usr/bin/env python3
"""Plot two store-stride replacement-policy experiments side by side."""

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
        "axes.labelsize": 14,
        "xtick.labelsize": 11,
        "ytick.labelsize": 11,
        "axes.linewidth": 0.8,
        "pdf.fonttype": 42,
        "ps.fonttype": 42,
    }
)


SCRIPT_DIR = Path(__file__).resolve().parent
DEFAULT_INPUT_A = SCRIPT_DIR / "rep-policy-1.txt"
DEFAULT_INPUT_B = SCRIPT_DIR / "rep-policy-2.txt"
DEFAULT_OUTPUT = SCRIPT_DIR / "rep-policy-plot.png"


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--input-a",
        type=Path,
        default=DEFAULT_INPUT_A,
        help="input for subplot (a) (default: %(default)s)",
    )
    parser.add_argument(
        "--input-b",
        type=Path,
        default=DEFAULT_INPUT_B,
        help="input for subplot (b) (default: %(default)s)",
    )
    parser.add_argument(
        "-o",
        "--output",
        type=Path,
        default=DEFAULT_OUTPUT,
        help="output base path; matching PNG and EPS files are generated",
    )
    parser.add_argument("--dpi", type=int, default=300, help="PNG output DPI")
    parser.add_argument(
        "--show", action="store_true", help="display the plot interactively"
    )
    return parser.parse_args()


def load_data(path: Path) -> tuple[list[int], list[float]]:
    if not path.is_file():
        raise FileNotFoundError(f"input file does not exist: {path}")

    pages: list[int] = []
    values: list[float] = []
    with path.open(encoding="utf-8") as input_file:
        header = input_file.readline().strip()
        if not header.startswith("test_page") or "l2d_cache_hwprf" not in header:
            raise ValueError(
                f"{path}: expected 'test_page' and 'l2d_cache_hwprf / round'"
            )

        for line_number, line in enumerate(input_file, start=2):
            fields = line.split()
            if not fields:
                continue
            if len(fields) != 2:
                raise ValueError(f"{path}:{line_number}: expected two columns")
            try:
                pages.append(int(fields[0]))
                values.append(float(fields[1]))
            except ValueError as error:
                raise ValueError(
                    f"{path}:{line_number}: invalid numeric value"
                ) from error

    order = sorted(range(len(pages)), key=pages.__getitem__)
    pages = [pages[index] for index in order]
    values = [values[index] for index in order]
    if not pages:
        raise ValueError(f"{path}: no data rows")
    if len(set(pages)) != len(pages):
        raise ValueError(f"{path}: duplicate test_page value")
    return pages, values


def draw_subplot(
    axis: plt.Axes,
    pages: list[int],
    values: list[float],
    panel_label: str,
) -> None:
    axis.plot(
        pages,
        values,
        color="#2166ac",
        linestyle="-",
        linewidth=1.7,
        marker="o",
        markersize=4.5,
        markerfacecolor="#2166ac",
        markeredgecolor="#174a7e",
        markeredgewidth=0.7,
        zorder=3,
    )
    axis.set_xlabel("test_page")
    axis.set_xlim(min(pages), max(pages))
    axis.set_xticks(pages)
    axis.yaxis.set_major_locator(MultipleLocator(2))
    axis.tick_params(axis="both", which="both", direction="in", top=True, right=True)
    axis.grid(axis="both", linestyle="--", linewidth=0.45, color="0.82")
    axis.set_axisbelow(True)
    axis.set_title(panel_label, fontsize=14, pad=3)


def draw_plot(
    data_a: tuple[list[int], list[float]],
    data_b: tuple[list[int], list[float]],
) -> plt.Figure:
    figure, axes = plt.subplots(1, 2, figsize=(7.4, 2.1), sharey=True)
    draw_subplot(axes[0], *data_a, "(a)")
    draw_subplot(axes[1], *data_b, "(b)")
    axes[0].set_ylabel("l2d_cache_hwprf")

    all_values = data_a[1] + data_b[1]
    upper_limit = max(2.0, max(all_values) + 0.6)
    axes[0].set_ylim(-0.5, upper_limit)
    figure.tight_layout(pad=0.45, w_pad=1.0)
    return figure


def main() -> None:
    args = parse_args()
    data_a = load_data(args.input_a)
    data_b = load_data(args.input_b)
    figure = draw_plot(data_a, data_b)

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
