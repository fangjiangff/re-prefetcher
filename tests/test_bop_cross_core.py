from pathlib import Path
import platform
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


@unittest.skipUnless(platform.machine() in ("aarch64", "arm64"), "AArch64 experiment")
class CrossCorePlanTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory()
        cls.binary = Path(cls.temp.name) / "cross-core"
        subprocess.run(["gcc", "-std=gnu11", "-O2", "-Wall", "-Wextra", "-Werror",
                        "-march=armv8.5-a+predres", "-pthread",
                        str(ROOT / "re3-bop/test-bop-cross-core.c"), "-o", str(cls.binary)], check=True)

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def describe(self, case):
        run = subprocess.run([str(self.binary), "--case", case, "--describe"],
                             text=True, capture_output=True, check=True)
        return dict(item.split("=") for item in run.stdout.split())

    def test_training_ownership_matches_controls(self):
        for case, a, b in [("untrained", 0, 0), ("local_n", 300, 0), ("local_nm", 600, 0),
                           ("remote_same", 300, 300), ("remote_other", 300, 300),
                           ("remote_only", 0, 300), ("no_trigger", 300, 300)]:
            row = self.describe(case)
            self.assertEqual(int(row["a_pairs"]), a)
            self.assertEqual(int(row["b_pairs"]), b)
            self.assertEqual(row["probe_cpu"], "6")
        self.assertEqual(self.describe("remote_other")["b_offset"], "13")

    def test_invalid_cpu_pair_or_bounds_rejected(self):
        for args in [("--cpu-b", "6"), ("--cpu-a", "1024"),
                     ("--train-b-pairs", "4097"), ("--offset-lines", "64"),
                     ("--other-offset", "16"), ("--rounds", "0"), ("--hold-us", "0")]:
            result = subprocess.run([str(self.binary), "--describe", *args], capture_output=True)
            self.assertEqual(result.returncode, 2)

    def test_no_training_is_valid(self):
        run = subprocess.run([str(self.binary), "--describe", "--train-a-pairs", "0",
                              "--train-b-pairs", "0"], text=True, capture_output=True, check=True)
        self.assertIn("a_pairs=0 b_pairs=0", run.stdout)
