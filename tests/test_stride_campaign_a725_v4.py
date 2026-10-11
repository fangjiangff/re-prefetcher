import unittest

from prefetch_agent.stride_campaign_a725_v4 import parse_index_mode_safe


class A725V4ParserTests(unittest.TestCase):
    def test_explanation_lines_are_not_data_rows(self):
        text = """
T0 sanity failed: explanatory text
T0_no_trigger 300 no 0.0 0.0
T0 50 yes 3.0 4.0
T1 52 yes 3.0 4.0
T2 250 no 0.0 0.0
T3 260 no 0.0 0.0
T4 270 no 0.0 0.0
"""
        result = parse_index_mode_safe(text)
        self.assertTrue(result["sanity_passed"])
        self.assertFalse(result["pc_equality_required_candidate"])
        self.assertTrue(result["va_equality_required_candidate"])


if __name__ == "__main__":
    unittest.main()
