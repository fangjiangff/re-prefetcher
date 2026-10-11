import tempfile
import unittest
from pathlib import Path

from prefetch_agent.stride_campaign_v2 import (
    parse_context_validity,
    parse_pc_bits_with_pmu,
)


class StrideCampaignV2Tests(unittest.TestCase):
    def test_pmu_only_pc_bits_are_classified(self):
        content = (
            "case\tlatency_ns\tprefetched\tthreshold_ns\tl1\tl2\n"
            "same_pc\t-1\tyes\t80\t1.0\t1.0\n"
            "bit_3\t-1\tno\t80\t0.3\t0.3\n"
            "bit_16\t-1\tyes\t80\t1.0\t1.0\n"
        )
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "pc.tsv"
            path.write_text(content)
            result = parse_pc_bits_with_pmu(path)
        self.assertEqual(result["sensitive_bits"], [3])
        self.assertEqual(result["insensitive_bits"], [16])
        self.assertEqual(result["unmapped_bits"], [])

    def test_invalid_context_baseline_is_inconclusive(self):
        text = """
Running cross-thread: command
Baselines valid: NO
Experiment3 proves prefetcher state survives context switch and is not flushed: YES
Experiment4 proves prefetcher state is shared across contexts: YES
Running cross-core: command
Baselines valid: YES
Experiment3 proves prefetcher state survives context switch and is not flushed: NO
Experiment4 proves prefetcher state is shared across contexts: NO
"""
        result = parse_context_validity(text)
        self.assertEqual(result["cross-thread"]["state"], "inconclusive")
        self.assertIsNone(result["cross-thread"]["survives_switch"])
        self.assertEqual(result["cross-core"]["state"], "observed")
        self.assertFalse(result["cross-core"]["shared_across_context"])


if __name__ == "__main__":
    unittest.main()
