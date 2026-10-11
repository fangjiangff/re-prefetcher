import importlib.util
import subprocess
import sys
import unittest
from pathlib import Path


REPO = Path(__file__).resolve().parent.parent
DRIVER = REPO / "re3-bop" / "bop-cc.py"


def load_driver():
    spec = importlib.util.spec_from_file_location("bop_cc_runner", DRIVER)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


class BopCcRunnerTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.driver = load_driver()

    def test_parse_output_extracts_rounds_and_summary(self):
        output = """# random_seed=123 rounds=4 train_pairs=10 trigger_batch=1 offset_lines=16 threshold_ns=80
rounds_elapsed_ns=123456 rounds_elapsed_ms=0.123456
# bits_transferred=4 bandwidth_bit_s=1200000000 bandwidth_kbit_s=1200000
round=0 trained=1 prefetch_events=5
round=1 trained=0 prefetch_events=1
condition=untrained rounds=1 l2_prefetch_events=1
condition=trained rounds=1 l2_prefetch_events=5
# bit_decisions=4 correct=3 errors=1 error_rate=0.250000 error_rate_percent=25.000
"""

        parsed = self.driver.parse_output(output)

        self.assertEqual(parsed["rounds"][0]["trained"], 1)
        self.assertEqual(parsed["elapsed_ns"], 123456)
        self.assertEqual(parsed["bandwidth"]["bandwidth_bit_s"], 1200000000.0)
        self.assertEqual(parsed["conditions"]["trained"]["l2_prefetch_events"], 5)
        self.assertEqual(parsed["conditions"]["untrained"]["l2_prefetch_events"], 1)
        self.assertEqual(parsed["error"]["errors"], 1)

    def test_describe_reports_single_trigger_and_pmu_mode(self):
        completed = subprocess.run(
            [sys.executable, str(DRIVER), "--describe"],
            text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
        )

        self.assertEqual(completed.returncode, 0, completed.stdout)
        self.assertIn("trigger_batch=1", completed.stdout)
        self.assertIn("measurement=pmu", completed.stdout)

    def test_default_rounds_is_one_thousand(self):
        args = self.driver.parse_args(["--describe"])
        self.assertEqual(args.rounds, 1000)

    def test_format_summary_returns_only_bandwidth_and_error_rate(self):
        parsed = {
            "bandwidth": {
                "bandwidth_bit_s": 15722.575,
                "bandwidth_kbit_s": 15.722575,
            },
            "error": {"error_rate_percent": 7.5},
        }

        self.assertEqual(
            self.driver.format_summary(parsed),
            ["bandwidth_bit_s=15722.575 bandwidth_kbit_s=15.723",
             "error_rate_percent=7.500"],
        )


if __name__ == "__main__":
    unittest.main()
