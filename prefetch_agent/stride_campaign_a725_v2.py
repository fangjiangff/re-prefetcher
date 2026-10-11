"""A725 campaign entry point with the corrected trigger PMU adapter."""

from __future__ import annotations

import sys

from . import stride_agent
from .stride_agent import CampaignError
from .stride_campaign_a725 import parse_a725_args
from .stride_campaign_v2 import Campaign


def main(argv: list[str] | None = None) -> int:
    stride_agent.TRIGGER = (
        stride_agent.REPO / "prefetch_agent" / "a725_trigger_adapter.py"
    )
    try:
        Campaign(parse_a725_args(argv)).execute()
    except CampaignError as exc:
        print(f"stride campaign error: {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
