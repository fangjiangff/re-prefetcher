#!/usr/bin/env python3
"""Plot the A725/X925 store trigger sweeps as two heatmaps."""

from __future__ import annotations
import base64
import zlib

import argparse
from io import BytesIO
from pathlib import Path

import matplotlib.pyplot as plt
import numpy as np
import seaborn as sns
from PIL import Image


SCRIPT_DIR = Path(__file__).resolve().parent
DATASETS = (
    # ("Store", "A725-core4-trigger-sweep-store-maxstride4096-maxstep40.txt"),
    ("Store", "X925-core6-trigger-sweep-store-maxstride4096-maxstep40.txt"),
    ("Load", "X925-core6-trigger-sweep-load-maxstride4096-maxstep40.txt"),
)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--vmax",
        type=float,
        default=200,
        help="heatmap color maximum (default: %(default)s)",
    )
    parser.add_argument(
        "--vmin",
        type=float,
        default=0,
        help="heatmap color minimum (default: %(default)s)",
    )
    parser.add_argument(
        "-o",
        "--output",
        type=Path,
        default=None,
        help="output one image instead of the default PNG and EPS pair",
    )
    parser.add_argument("--dpi", type=int, default=300, help="output DPI")
    parser.add_argument(
        "--show", action="store_true", help="also display the plot interactively"
    )
    return parser.parse_args()


def load_heatmap(path: Path) -> np.ndarray:
    """Load avg_ticks into [access, stride], restricted to 1..40 and 1..32."""
    data = np.loadtxt(path, comments="#")
    heatmap = np.full((40, 32), np.nan)

    for stride, access, avg_ticks in data:
        stride_i = int(stride)
        access_i = int(access)
        if 1 <= stride_i <= 32 and 1 <= access_i <= 40:
            heatmap[access_i - 1, stride_i - 1] = avg_ticks

    return heatmap


def save_raster_eps(output: Path, png_buffer: BytesIO, dpi: int) -> None:
    """Wrap the exact rendered PNG pixels in a lossless, correctly sized EPS."""
    png_buffer.seek(0)
    with Image.open(png_buffer) as png_image:
        rgb_image = png_image.convert("RGB")
        width, height = rgb_image.size
        compressed = zlib.compress(rgb_image.tobytes(), level=9)

    width_points = width * 72.0 / dpi
    height_points = height * 72.0 / dpi
    bbox_width = (width * 72 + dpi - 1) // dpi
    bbox_height = (height * 72 + dpi - 1) // dpi
    encoded = base64.a85encode(compressed).decode("ascii")
    encoded_lines = "\n".join(
        encoded[offset : offset + 100] for offset in range(0, len(encoded), 100)
    )

    eps = f"""%!PS-Adobe-3.0 EPSF-3.0
%%Creator: plot-trigger.py (lossless PNG raster wrapper)
%%BoundingBox: 0 0 {bbox_width} {bbox_height}
%%HiResBoundingBox: 0 0 {width_points:.6f} {height_points:.6f}
%%LanguageLevel: 3
%%Pages: 1
%%EndComments
gsave
{width_points:.6f} {height_points:.6f} scale
/DeviceRGB setcolorspace
<<
  /ImageType 1
  /Width {width}
  /Height {height}
  /BitsPerComponent 8
  /Decode [0 1 0 1 0 1]
  /ImageMatrix [{width} 0 0 -{height} 0 {height}]
  /DataSource currentfile /ASCII85Decode filter /FlateDecode filter
>>
image
{encoded_lines}
~>
grestore
showpage
%%EOF
"""
    output.write_text(eps, encoding="ascii")


def main() -> None:
    args = parse_args()
    heatmaps = [load_heatmap(SCRIPT_DIR / filename) for _, filename in DATASETS]

    if not any(np.isfinite(values).any() for values in heatmaps):
        raise ValueError("no data found in stride 1..32 and access 1..40")

    if args.vmax <= args.vmin:
        raise ValueError(f"vmax ({args.vmax}) must be greater than vmin ({args.vmin})")

    fig, axes = plt.subplots(1, 2, figsize=(20, 8.3))
    fig.subplots_adjust(
        left=0.08, right=0.95, bottom=0.22, top=0.90, wspace=0.12
    )
    stride_labels = np.arange(1, 33)
    access_labels = np.arange(1, 41)

    for index, (axis, (title, _), heatmap) in enumerate(
        zip(axes, DATASETS, heatmaps)
    ):
        sns.heatmap(
            np.ma.masked_invalid(heatmap.T),
            cmap="RdYlBu_r",
            annot=False,
            ax=axis,
            vmin=args.vmin,
            vmax=args.vmax,
            cbar=False,
            xticklabels=access_labels,
            yticklabels=stride_labels,
        )
        axis.set_title(title, loc="center", pad=10, fontsize=40)
        axis.set_xlabel("Access count", fontsize=34)
        axis.set_ylabel("Stride (cache lines)" if index == 0 else "", fontsize=34)
        axis.set_xticks(np.arange(1, 40, 2) + 0.5)
        axis.set_xticklabels(access_labels[1::2], fontsize=28, rotation=90)
        axis.set_yticks(np.arange(1, 32, 2) + 0.5)
        axis.set_yticklabels(stride_labels[1::2], fontsize=28, rotation=0)
        axis.tick_params(axis="x", labelsize=28)
        axis.tick_params(axis="y", labelsize=28)

    colorbar = fig.colorbar(
        axes[-1].collections[0],
        ax=axes.tolist(),
        location="right",
        pad=0.015,
        fraction=0.025,
        aspect=25,
    )
    colorbar.set_label("Average latency (ticks)", fontsize=34)
    colorbar.ax.tick_params(labelsize=28)

    outputs = (
        [args.output]
        if args.output is not None
        else [SCRIPT_DIR / "trigger-heatmap.png", SCRIPT_DIR / "trigger-heatmap.eps"]
    )
    png_buffer = BytesIO()
    fig.savefig(
        png_buffer,
        format="png",
        dpi=args.dpi,
        bbox_inches="tight",
        pad_inches=0,
    )
    png_bytes = png_buffer.getvalue()

    for output in outputs:
        output.parent.mkdir(parents=True, exist_ok=True)
        suffix = output.suffix.lower()
        if suffix == ".png":
            output.write_bytes(png_bytes)
        elif suffix in {".eps", ".ps"}:
            save_raster_eps(output, png_buffer, args.dpi)
        else:
            fig.savefig(output, dpi=args.dpi, bbox_inches="tight", pad_inches=0)
        print(f"Saved {output}")

    if args.show:
        plt.show()
    else:
        plt.close(fig)


if __name__ == "__main__":
    main()
