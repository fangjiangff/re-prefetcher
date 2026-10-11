import tempfile
import unittest
from pathlib import Path

from prefetch_agent.stride_agent import (
    first_cache_activation,
    first_replicated_pmu_activation,
    first_stable_pmu_activation,
    parse_degree,
    parse_index_mode,
)


class StrideAgentTests(unittest.TestCase):
    def test_activation_rules(self):
        values = {2: [0.0, 0.0, 0.0], 3: [1.0, 0.1, 2.0], 5: [3.0, 3.1, 2.9]}
        self.assertEqual(first_replicated_pmu_activation(values), 3)
        self.assertEqual(first_stable_pmu_activation(values), 5)
        self.assertEqual(first_cache_activation([
            {"train_step": "2", "avg_ns": "200"},
            {"train_step": "3", "avg_ns": "32"},
        ], 80), 3)

    def test_index_sanity_and_candidates(self):
        text = """
test avg_ticks prefetched l1 l2
T0_no_trigger 200 no 0 0
T0 32 yes 2 3
T1 180 no 0 0
T2 35 yes 2 2
T3 190 no 0 0
T4 34 yes 2 2
"""
        result = parse_index_mode(text)
        self.assertTrue(result["sanity_passed"])
        self.assertTrue(result["pc_equality_required_candidate"])
        self.assertFalse(result["va_equality_required_candidate"])
        self.assertTrue(result["pa_sufficient_candidate"])

    def test_degree_is_consecutive_from_first_candidate(self):
        content = (
            "train_step\tprobe_pos\toffset_bytes\trole\tavg_ns\n"
            "3\t3\t576\tcandidate\t30\n"
            "3\t4\t768\tcandidate\t32\n"
            "3\t5\t960\tcandidate\t200\n"
            "3\t6\t1152\tcandidate\t31\n"
        )
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "degree.tsv"
            path.write_text(content)
            result = parse_degree(path, 3, 80)
        self.assertEqual(result["consecutive_degree"], 2)
        self.assertEqual(result["all_cached_candidate_count"], 3)


if __name__ == "__main__":
    unittest.main()
