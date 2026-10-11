"""Cortex-A725 CPU4 entry point for the Load/Store stride campaign."""

from __future__ import annotations

import sys

from .stride_agent import CampaignError, parse_args
from .stride_campaign_v2 import Campaign


def parse_a725_args(argv: list[str] | None = None):
    original = list(sys.argv[1:] if argv is None else argv)
    patched = list(original)
    if "--arch" in patched:
        position = patched.index("--arch")
        if position + 1 >= len(patched) or patched[position + 1] != "A725":
            raise CampaignError("this entry point only accepts --arch A725")
        patched[position + 1] = "X925"
    args = parse_args(patched)
    args.arch = "A725"
    if "--core" not in original:
        args.core = 4
    if "--hit-threshold" not in original:
        args.hit_threshold = 120
    return args


def main(argv: list[str] | None = None) -> int:
    try:
        Campaign(parse_a725_args(argv)).execute()
    except CampaignError as exc:
        print(f"stride campaign error: {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
