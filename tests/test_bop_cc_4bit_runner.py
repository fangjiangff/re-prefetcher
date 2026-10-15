import importlib.util
import subprocess
import sys
import unittest
from pathlib import Path


REPO = Path(__file__).resolve().parent.parent
DRIVER = REPO / "re3-bop" / "bop-cc-4bit.py"


def load_driver():
    spec = importlib.util.spec_from_file_location("bop_cc_4bit_runner", DRIVER)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


class BopCc4BitRunnerTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.driver = load_driver()

    def test_parse_output_extracts_error_and_histogram(self):
        output = """# rounds_elapsed_ns=1000000 rounds_elapsed_ms=1.0
# bits_transferred=50 bandwidth_bit_s=50000 bandwidth_kbit_s=50
# correct=8 errors=2 error_rate=0.2 error_rate_percent=20.0
"""

        parsed = self.driver.parse_output(output)

        self.assertEqual(parsed["error"]["errors"], 2)
        self.assertEqual(parsed["error"]["error_rate"], 0.2)
        self.assertEqual(parsed["bandwidth"]["bits_transferred"], 50)

    def test_describe_reports_range_and_cache_probe(self):
        completed = subprocess.run(
            [sys.executable, str(DRIVER), "--describe"],
            text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
        )

        self.assertEqual(completed.returncode, 0, completed.stdout)
        self.assertIn("random_value_range=1..32", completed.stdout)
        self.assertIn("measurement=cache-probe", completed.stdout)

    def test_format_summary_contains_only_bandwidth_and_error(self):
        parsed = {
            "error": {"error_rate_percent": 7.5},
            "bandwidth": {"bandwidth_bit_s": 15722.575,
                          "bandwidth_kbit_s": 15.723},
        }

        lines = self.driver.format_summary(parsed)

        self.assertEqual(lines, [
            "bandwidth_bit_s=15722.575 bandwidth_kbit_s=15.723",
            "error_rate_percent=7.500",
        ])


if __name__ == "__main__":
    unittest.main()
