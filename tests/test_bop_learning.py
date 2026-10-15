import importlib.util
from pathlib import Path
import platform
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


@unittest.skipUnless(platform.machine() in ("aarch64", "arm64"), "AArch64 code")
class LearningSequenceTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory()
        cls.binary = Path(cls.temp.name) / "learning"
        subprocess.run(["gcc", "-std=gnu11", "-O2", "-Wall", "-Wextra", "-Werror",
                        "-march=armv8.5-a+predres", str(ROOT / "re3-bop/test-bop-learning.c"),
                        "-o", str(cls.binary)], check=True)

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def describe(self, *args):
        result = subprocess.run([str(self.binary), "--describe", *args],
                                capture_output=True, text=True, check=True)
        return dict(field.split("=") for field in result.stdout.split())

    def test_pc_assignment_preserves_exact_address_sequence(self):
        for pattern in ("pair", "stride", "chain", "random"):
            a = self.describe("--pattern", pattern, "--pc", "single")
            b = self.describe("--pattern", pattern, "--pc", "random")
            c = self.describe("--pattern", pattern, "--pc", "rotate")
            self.assertEqual(a["address_hash"], b["address_hash"])
            self.assertEqual(a["address_hash"], c["address_hash"])
            self.assertEqual(a["pc_mask"], "1")
            self.assertEqual(c["pc_mask"], "65535")

    def test_equal_loads_are_not_equal_pair_counts(self):
        stride = self.describe("--pattern", "stride")
        pair = self.describe("--pattern", "pair")
        self.assertEqual(stride["stage_b_loads"], pair["stage_b_loads"])
        self.assertEqual(stride["adjacent_d"], "599")
        self.assertEqual(pair["adjacent_d"], "300")

    def test_unsafe_or_ambiguous_options_rejected(self):
        for args in (("--loads", "601"), ("--pages", "0"), ("--offset", "64"),
                     ("--loads", "-1"), ("--cpu", "1024"),
                     ("--pattern", "stride", "--loads", "8192", "--offset", "63")):
            result = subprocess.run([str(self.binary), "--describe", *args],
                                    capture_output=True)
            self.assertEqual(result.returncode, 2)

    def test_zero_initial_training_supported(self):
        row = self.describe("--initial-pairs", "0", "--loads", "0")
        self.assertEqual(row["initial_loads"], "0")
        self.assertEqual(row["stage_b_loads"], "0")

    def test_skip_or_limit_keeps_prepared_address_sequence(self):
        full = self.describe("--resident", "hw-offset")
        skip = self.describe("--resident", "hw-offset", "--skip-b")
        limit = self.describe("--resident", "hw-offset", "--b-limit", "32")
        self.assertEqual(full["address_hash"], skip["address_hash"])
        self.assertEqual(full["address_hash"], limit["address_hash"])
        self.assertEqual(skip["executed_loads"], "0")
        self.assertEqual(limit["executed_loads"], "32")

    def test_invalid_residency_and_limit_rejected(self):
        for args in (("--resident", "unknown"), ("--evict-kb", "1025"),
                     ("--b-limit", "601"), ("--b-limit", "1000")):
            result = subprocess.run([str(self.binary), "--describe", *args], capture_output=True)
            self.assertEqual(result.returncode, 2)


class LearningSummaryTests(unittest.TestCase):
    def test_percent_is_not_per_mille(self):
        spec = importlib.util.spec_from_file_location("learning_runner", ROOT / "re3-bop/test-bop-learning.py")
        mod = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(mod)
        rows = [dict(case="x", measure="cache", hit_pct=value, avg_ns=32,
                     train_hit_pct=0, train_avg_ns=0, l1_per_load=0, l2_per_load=0,
                     l1_miss_per_load=0, l2_miss_per_load=0) for value in (90, 100)]
        summary = mod.summarize(rows)[0]
        self.assertEqual(summary["hit_pct_mean"], 95)
        self.assertEqual(summary["hit_pct_min"], 90)
        self.assertEqual(summary["hit_pct_max"], 100)
