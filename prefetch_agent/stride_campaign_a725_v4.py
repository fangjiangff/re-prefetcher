"""A725 campaign with PMU/cache activation reconciliation and safe parsing."""

from __future__ import annotations

import sys

from . import stride_agent
from .stride_agent import CampaignError, clean_ansi
from .stride_campaign_a725 import parse_a725_args
from .stride_campaign_v2 import Campaign as BaseCampaign


def parse_index_mode_safe(text: str) -> dict:
    rows = {}
    names = {"T0_no_trigger", "T0", "T1", "T2", "T3", "T4"}
    for raw in clean_ansi(text).splitlines():
        fields = raw.split()
        if len(fields) < 3 or fields[0] not in names:
            continue
        try:
            latency = int(fields[1])
        except ValueError:
            continue
        rows[fields[0]] = {
            "latency": latency,
            "prefetched": fields[2] == "yes",
            "counters": fields[3:],
        }
    required = names
    sanity = (
        required.issubset(rows)
        and not rows["T0_no_trigger"]["prefetched"]
        and rows["T0"]["prefetched"]
    )
    result = {"rows": rows, "sanity_passed": sanity}
    if not sanity:
        result.update({
            "state": "inconclusive",
            "reason": "T0_no_trigger/T0 sanity gate failed or rows are missing",
        })
        return result
    result.update({
        "state": "observed",
        "pc_equality_required_candidate": not rows["T1"]["prefetched"],
        "va_equality_required_candidate": not rows["T2"]["prefetched"],
        "same_va_different_pa_cross_process": rows["T3"]["prefetched"],
        "pa_sufficient_candidate": rows["T4"]["prefetched"],
        "limitations": [
            "T3 changes both PA and process context",
            "sensitivity identifies match/index/tag/hash input, not an index bit alone",
        ],
    })
    return result


class Campaign(BaseCampaign):
    def reconcile_activation(self) -> None:
        profiles = (
            self.state.get("phases", {}).get("existence", {})
            .get("access_profiles", {})
        )
        for profile in profiles.values():
            cache = profile.get("cache_first_accesses")
            pmu = profile.get("pmu_first_replicated_accesses")
            if cache is not None and pmu is not None:
                profile["minimum_accesses"] = max(cache, pmu)
                profile["activation_reconciliation"] = (
                    "maximum of cache-first and PMU-majority-first"
                )
        self.save()

    def execute(self) -> None:
        self.reconcile_activation()
        super().execute()


def main(argv: list[str] | None = None) -> int:
    stride_agent.TRIGGER = (
        stride_agent.REPO / "prefetch_agent" / "a725_trigger_adapter_v2.py"
    )
    stride_agent.parse_index_mode = parse_index_mode_safe
    try:
        Campaign(parse_a725_args(argv)).execute()
    except CampaignError as exc:
        print(f"stride campaign error: {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
