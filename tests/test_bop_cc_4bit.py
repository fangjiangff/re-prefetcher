import platform
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path


class BopCc4BitTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.repo = Path(__file__).resolve().parent.parent
        cls.temp = tempfile.TemporaryDirectory()
        cls.binary = Path(cls.temp.name) / "bop-cc-4bit"
        compiler = (
            "gcc" if platform.machine().lower() in ("aarch64", "arm64")
            else shutil.which("aarch64-linux-gnu-gcc")
        )
        if compiler is None:
            raise unittest.SkipTest("an AArch64 C compiler is required")
        cls.build = subprocess.run(
            [compiler, "-std=gnu11", "-O2", "-Wall", "-Wextra", "-Werror",
             "-Wno-unused-result", "-march=armv8.5-a+predres",
             "-DENABLE_CPP_RCTX=1", "-DENABLE_PMU=0", "-DENABLE_CACHE_PROBE=1",
             str(cls.repo / "re3-bop" / "bop-cc-4bit.c"),
             str(cls.repo / "re0-stride-prefetcher" / "until.c"),
             "-o", str(cls.binary)], text=True,
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
        )

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def test_build_succeeds(self):
        self.assertEqual(self.build.returncode, 0, self.build.stdout)

    def test_probe_inference_reports_error_rate(self):
        if platform.machine().lower() not in ("aarch64", "arm64"):
            self.skipTest("running the AArch64 binary requires AArch64")
        self.assertEqual(self.build.returncode, 0, self.build.stdout)
        completed = subprocess.run(
            [str(self.binary), "4", "80", "4", "123"],
            text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
        )
        self.assertEqual(completed.returncode, 0, completed.stdout)
        self.assertIn("random_value_range=1..32", completed.stdout)
        self.assertIn("probe_offsets=1..32", completed.stdout)
        self.assertIn("error_rate=", completed.stdout)
        self.assertIn("correct=", completed.stdout)
        self.assertIn("bits_transferred=20", completed.stdout)
        self.assertIn("bandwidth_bit_s=", completed.stdout)
        self.assertNotIn("round=", completed.stdout)


if __name__ == "__main__":
    unittest.main()
