import platform
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path


class BopConditionalTrainingTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.repo = Path(__file__).resolve().parent.parent
        cls.temp = tempfile.TemporaryDirectory()
        cls.binary = Path(cls.temp.name) / "bop-cc"
        compiler = (
            "gcc" if platform.machine().lower() in ("aarch64", "arm64")
            else shutil.which("aarch64-linux-gnu-gcc")
        )
        if compiler is None:
            raise unittest.SkipTest("an AArch64 C compiler is required")
        cls.build = subprocess.run(
            [compiler, "-std=gnu11", "-O2", "-Wall", "-Wextra", "-Werror",
             "-Wno-unused-result", "-march=armv8.5-a+predres",
             "-DENABLE_CPP_RCTX=1", "-DENABLE_PMU=1", "-DENABLE_CACHE_PROBE=0",
             str(cls.repo / "re3-bop" / "bop-cc.c"),
             str(cls.repo / "re0-stride-prefetcher" / "until.c"),
             "-o", str(cls.binary)], text=True,
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
        )

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def test_build_succeeds(self):
        self.assertEqual(self.build.returncode, 0, self.build.stdout)

    def test_two_conditions_are_reported(self):
        if platform.machine().lower() not in ("aarch64", "arm64"):
            self.skipTest("running the AArch64 binary requires AArch64")
        self.assertEqual(self.build.returncode, 0, self.build.stdout)
        completed = subprocess.run(
            [str(self.binary), "4", "80", "4", "1", "16", "123"],
            text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
        )
        if completed.returncode != 0 and "PMU" in completed.stdout:
            self.skipTest(f"PMU unavailable:\n{completed.stdout}")
        self.assertEqual(completed.returncode, 0, completed.stdout)
        self.assertIn("# random_seed=123", completed.stdout)
        self.assertIn("trigger_batch=1", completed.stdout)
        self.assertIn("condition=trained", completed.stdout)
        self.assertIn("condition=untrained", completed.stdout)
        self.assertIn("l2_prefetch_events=", completed.stdout)
        self.assertNotIn("l1_prefetches=", completed.stdout)
        self.assertNotIn("l2_prefetches=", completed.stdout)
        self.assertIn("rounds_elapsed_ns=", completed.stdout)
        self.assertIn("bandwidth_bit_s=", completed.stdout)
        self.assertIn("bandwidth_kbit_s=", completed.stdout)
        self.assertNotIn("round=", completed.stdout)
        self.assertIn("classification uses PMU", completed.stdout)
        self.assertIn("error_rate=", completed.stdout)
        self.assertIn("inferred condition counts sum to rounds", completed.stdout)


if __name__ == "__main__":
    unittest.main()
