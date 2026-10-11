"""Correctness-gated CLI for the single-agent Load/Store stride campaign."""

from __future__ import annotations

import re
import sys
from pathlib import Path

from .stride_agent import CampaignError, PHASES, clean_ansi, parse_args, read_tsv
from .stride_campaign import Campaign as BaseCampaign


def parse_pc_bits_with_pmu(path: Path) -> dict:
    """Classify PC bits from the benchmark verdict, including PMU-only runs."""
    rows = read_tsv(path)
    by_case = {row["case"]: row for row in rows}
    same = by_case.get("same_pc")
    if same is None or same.get("prefetched") != "yes":
        return {"state": "inconclusive", "reason": "same_pc positive control failed"}

    sensitive, insensitive = [], []
    for name, row in by_case.items():
        if not name.startswith("bit_"):
            continue
        bit = int(name.split("_", 1)[1])
        (insensitive if row.get("prefetched") == "yes" else sensitive).append(bit)
    return {
        "state": "observed",
        "sensitive_bits": sensitive,
        "insensitive_bits": insensitive,
        "unmapped_bits": [],
        "meaning": "sensitive bits participate in PC match/index/tag/hash input",
        "measurement": "PMU verdict when cache probing is disabled",
        "pairwise_hash_state": "not_implemented_by_existing_PC_benchmark",
    }


def parse_context_validity(text: str) -> dict[str, dict]:
    """Parse each context test, requiring both positive and negative baselines."""
    plain = clean_ansi(text)
    starts = list(re.finditer(r"^Running (cross-[a-z0-9-]+):", plain, re.M))
    observations = {}
    for index, start in enumerate(starts):
        name = start.group(1)
        end = starts[index + 1].start() if index + 1 < len(starts) else len(plain)
        block = plain[start.start():end]
        baseline = re.search(r"Baselines valid:\s+(YES|NO)", block)
        survives = re.search(
            r"state survives context switch and is not flushed:\s+(YES|NO)", block
        )
        shared = re.search(r"state is shared across contexts:\s+(YES|NO)", block)
        valid = bool(baseline and baseline.group(1) == "YES")
        observations[name] = {
            "state": "observed" if valid else "inconclusive",
            "baseline_valid": valid,
            "survives_switch": survives.group(1) == "YES" if valid and survives else None,
            "shared_across_context": shared.group(1) == "YES" if valid and shared else None,
        }
    return observations


class Campaign(BaseCampaign):
    def phase_index(self) -> None:
        super().phase_index()
        phase = self.state.get("phases", {}).get("index", {})
        for access, result in phase.get("access_profiles", {}).items():
            pc_path = self.output / "index" / f"{access}-pc-bits.tsv"
            if pc_path.is_file():
                result["pc_bits"] = parse_pc_bits_with_pmu(pc_path)
        self.save()

    def phase_context(self) -> None:
        super().phase_context()
        phase = self.state.get("phases", {}).get("context", {})
        for access, result in phase.get("access_profiles", {}).items():
            log_path = self.output / "logs" / f"context-{access}.stdout.txt"
            if not log_path.is_file():
                continue
            observations = parse_context_validity(log_path.read_text())
            result["contexts"] = observations
            result["state"] = (
                "observed" if any(item["baseline_valid"] for item in observations.values())
                else "inconclusive"
            )
            result["limitations"] = [
                "conclusions are emitted only when positive and negative baselines pass",
                "survival and cross-context sharing are separate properties",
                "TrustZone is skipped unless OP-TEE availability is explicitly established",
            ]
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
