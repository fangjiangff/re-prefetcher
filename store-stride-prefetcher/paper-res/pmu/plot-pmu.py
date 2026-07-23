#!/usr/bin/env python3
"""Plot L1D/L2D hardware-prefetch PMU values versus access count."""

from __future__ import annotations

import argparse
import csv
from pathlib import Path

import matplotlib.pyplot as plt
from matplotlib.ticker import MultipleLocator

plt.rcParams.update(
    {
        "font.family": "serif",
        "font.serif": ["Times New Roman", "Liberation Serif", "Nimbus Roman", "DejaVu Serif"],
        "font.size": 12,
        "axes.labelsize": 14,
        "legend.fontsize": 11,
        "xtick.labelsize": 11,
        "ytick.labelsize": 11,
        "axes.linewidth": 0.8,
        "pdf.fonttype": 42,
        "ps.fonttype": 42,
    }
)


SCRIPT_DIR = Path(__file__).resolve().parent
DEFAULT_INPUT = SCRIPT_DIR / "pmu-x925.txt"
DEFAULT_OUTPUT = SCRIPT_DIR / "pmu-line-scatter.png"
ACCESS_TYPES = ("load", "store")
METRICS = ("l1d_cache_hwprf", "l2d_cache_hwprf")


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "-i",
        "--input",
        type=Path,
        default=DEFAULT_INPUT,
        help="input PMU table (default: %(default)s)",
    )
    parser.add_argument(
        "-o",
        "--output",
        type=Path,
        default=DEFAULT_OUTPUT,
        help="output base path; matching .png and .eps files are written (default: %(default)s)",
    )
    parser.add_argument("--dpi", type=int, default=300, help="output DPI")
    parser.add_argument(
        "--show", action="store_true", help="also display the plot interactively"
    )
    return parser.parse_args()


def load_data(path: Path) -> dict[str, dict[str, list[float]]]:
    """Read and validate access counts 0--30 for load and store rows."""
    if not path.is_file():
        raise FileNotFoundError(f"input file does not exist: {path}")

    data = {
        access: {"count": [], **{metric: [] for metric in METRICS}}
        for access in ACCESS_TYPES
    }

    with path.open(encoding="utf-8", newline="") as input_file:
        reader = csv.DictReader(input_file, delimiter="\t")
        required = {"train_step", "access", *METRICS}
        missing = required.difference(reader.fieldnames or [])
        if missing:
            raise ValueError(f"{path}: missing columns: {', '.join(sorted(missing))}")

        for line_number, row in enumerate(reader, start=2):
            access = row["access"].strip().lower()
            if access not in data:
                continue
            try:
                count = int(row["train_step"])
                values = {metric: float(row[metric]) for metric in METRICS}
            except (TypeError, ValueError) as error:
                raise ValueError(f"{path}:{line_number}: invalid numeric value") from error

            if 0 <= count <= 30:
                data[access]["count"].append(count)
                for metric, value in values.items():
                    data[access][metric].append(value)

    expected_counts = list(range(0, 31))
    for access in ACCESS_TYPES:
        order = sorted(
            range(len(data[access]["count"])),
            key=data[access]["count"].__getitem__,
        )
        for key in ("count", *METRICS):
            data[access][key] = [data[access][key][index] for index in order]
        if data[access]["count"] != expected_counts:
            raise ValueError(
                f"{path}: {access} rows must contain each access count from 0 to 30"
            )

    return data


def draw_plot(data: dict[str, dict[str, list[float]]]) -> plt.Figure:
    fig, axis = plt.subplots(figsize=(7.2, 2.53))
    styles = {
        ("load", "l1d_cache_hwprf"): ("#b2182b", "o", "--"),
        ("load", "l2d_cache_hwprf"): ("#ef8a62", "s", "-"),
        ("store", "l1d_cache_hwprf"): ("#2166ac", "o", "--"),
        ("store", "l2d_cache_hwprf"): ("#67a9cf", "s", "-"),
    }

    for access in ACCESS_TYPES:
        for metric in METRICS:
            color, marker, linestyle = styles[(access, metric)]
            axis.plot(
                data[access]["count"],
                data[access][metric],
                color=color,
                linestyle=linestyle,
                linewidth=1.5,
                marker=marker,
                markersize=4.5,
                markerfacecolor=color if metric == "l1d_cache_hwprf" else "white",
                markeredgewidth=1.0,
                # Keep L1 visible when its values overlap the L2 curve.
                zorder=3 if metric == "l1d_cache_hwprf" else 2,
                label=f"{access} - {metric}",
            )

    axis.set_xlabel("Access count")
    axis.set_ylabel("PMU value")
    axis.yaxis.set_major_locator(MultipleLocator(5))
    axis.set_xlim(0, 30)
    axis.set_xticks(range(0, 31))
    axis.tick_params(axis="both", which="both", direction="in", top=True, right=True)
    axis.tick_params(axis="x", labelrotation=45)
    axis.grid(axis="both", linestyle="--", linewidth=0.45, color="0.82")
    axis.set_axisbelow(True)
    axis.legend(loc="upper left", ncol=2, frameon=True, fancybox=False, edgecolor="black", framealpha=1.0)
    fig.tight_layout(pad=0.5)
    return fig


def main() -> None:
    args = parse_args()
    data = load_data(args.input)
    figure = draw_plot(data)

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
