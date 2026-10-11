"""CLI entry point for the single-agent Load/Store stride campaign."""

from __future__ import annotations

import sys

from .stride_agent import CampaignError, PHASES, StrideCampaign, parse_args


class Campaign(StrideCampaign):
    def execute(self) -> None:
        handlers = {
            "existence": self.phase_existence,
            "range": self.phase_range,
            "degree": self.phase_degree,
            "index": self.phase_index,
            "capacity": self.phase_capacity,
            "context": self.phase_context,
        }
        selected = PHASES if self.args.phases == ["all"] else tuple(self.args.phases)
        if "existence" not in selected and "existence" not in self.state.get("phases", {}):
            raise CampaignError("existence phase must be run before dependent phases")
        for phase in selected:
            if phase != "existence" and not self.active_profiles():
                self.state["phases"][phase] = {
                    "state": "blocked", "reason": "no supported Load/Store profile",
                }
                self.save()
                continue
            print(f"\n## phase: {phase}", flush=True)
            handlers[phase]()
        self.state["status"] = "completed" if self.args.execute else "planned"
        self.save()


def main(argv: list[str] | None = None) -> int:
    args = parse_args(argv)
    try:
        Campaign(args).execute()
    except CampaignError as exc:
        print(f"stride campaign error: {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
