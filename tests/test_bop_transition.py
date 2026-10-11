import importlib.util
import subprocess
import sys
import unittest
from pathlib import Path


REPO = Path(__file__).resolve().parent.parent
DRIVER = REPO / "re3-bop" / "test-bop-transition.py"


def load_driver():
    spec = importlib.util.spec_from_file_location("bop_transition", DRIVER)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


class BopTransitionAnalysisTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.driver = load_driver()

    def test_parse_transition_result_selects_transition_row(self):
        output = """# metadata
case\ttrain_a\ttrain_b\thits_a_per_1000\thits_b_per_1000\tavg_ns_a\tavg_ns_b
transition_a_then_b\t3000\t420\t800.00\t700.00\t40.00\t45.00
"""

        row = self.driver.parse_transition_result(output)

        self.assertEqual(row["train_a"], 3000)
        self.assertEqual(row["train_b"], 420)
        self.assertEqual(row["hits_a_per_1000"], 800.0)

    def test_summarize_distinguishes_crossover_dominance_and_stability(self):
        records = [
            {"train_b": 420, "hits_a_per_1000": 300.0,
             "hits_b_per_1000": 800.0},
            {"train_b": 420, "hits_a_per_1000": 200.0,
             "hits_b_per_1000": 950.0},
        ]

        point = self.driver.summarize_point(records, old_max=250, new_min=900)

        self.assertTrue(point["crossover"])
        self.assertTrue(point["dominant"])
        self.assertFalse(point["stable_raw"])
        self.assertEqual(point["mean_hits_a_per_1000"], 250.0)
        self.assertEqual(point["mean_hits_b_per_1000"], 875.0)

    def test_stable_boundary_requires_two_following_points(self):
        points = [
            {"train_b": 300, "crossover": False, "dominant": False,
             "stable_raw": True},
            {"train_b": 320, "crossover": False, "dominant": False,
             "stable_raw": False},
            {"train_b": 400, "crossover": True, "dominant": False,
             "stable_raw": False},
            {"train_b": 420, "crossover": True, "dominant": True,
             "stable_raw": True},
            {"train_b": 440, "crossover": True, "dominant": True,
             "stable_raw": True},
            {"train_b": 460, "crossover": True, "dominant": True,
             "stable_raw": True},
        ]

        boundaries = self.driver.find_boundaries(points, confirmation_points=2)

        self.assertEqual(boundaries["crossover_pairs"], 400)
        self.assertEqual(boundaries["dominant_pairs"], 420)
        self.assertEqual(boundaries["stable_replacement_pairs"], 420)

    def test_old_best_must_be_visible_at_zero_new_training(self):
        established = self.driver.old_best_established(
            [{"train_b": 0, "min_hits_a_per_1000": 910.0,
              "max_hits_b_per_1000": 200.0}],
            old_max=250, new_min=900,
        )
        missing = self.driver.old_best_established(
            [{"train_b": 0, "min_hits_a_per_1000": 700.0,
              "max_hits_b_per_1000": 200.0}],
            old_max=250, new_min=900,
        )

        self.assertTrue(established)
        self.assertFalse(missing)

    def test_failed_baseline_invalidates_all_replacement_boundaries(self):
        points = [
            {"train_b": 0, "crossover": False, "dominant": False,
             "stable_raw": False, "min_hits_a_per_1000": 700.0,
             "max_hits_b_per_1000": 100.0},
            {"train_b": 100, "crossover": True, "dominant": True,
             "stable_raw": True, "min_hits_a_per_1000": 0.0,
             "max_hits_b_per_1000": 1000.0},
        ]

        boundaries = self.driver.validated_boundaries(
            points, confirmation_points=0, old_max=250, new_min=900
        )

        self.assertFalse(boundaries["old_best_established"])
        self.assertIsNone(boundaries["crossover_pairs"])
        self.assertIsNone(boundaries["dominant_pairs"])
        self.assertIsNone(boundaries["stable_replacement_pairs"])

    def test_fine_scan_spans_crossover_through_confirmed_stability(self):
        coarse = [
            {"train_b": 0, "crossover": False, "stable_raw": False},
            {"train_b": 100, "crossover": True, "stable_raw": False},
            {"train_b": 200, "crossover": True, "stable_raw": True},
        ]

        values = self.driver.choose_fine_values(
            coarse, start=0, stop=400, coarse_step=100, fine_step=20
        )

        self.assertEqual(values[0], 0)
        self.assertEqual(values[-1], 400)
        self.assertEqual(values[1] - values[0], 20)


class BopTransitionCliTests(unittest.TestCase):
    def test_describe_shows_default_coprime_offsets_and_coarse_schedule(self):
        completed = subprocess.run(
            [sys.executable, str(DRIVER), "--describe",
             "--coarse-stop", "200"],
            text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
        )

        self.assertEqual(completed.returncode, 0, completed.stdout)
        self.assertIn("offset_a=13 offset_b=16", completed.stdout)
        self.assertIn("stage\ttrain_b_pairs", completed.stdout)
        self.assertIn("coarse\t0", completed.stdout)
        self.assertIn("coarse\t200", completed.stdout)

    def test_non_coprime_offsets_are_rejected(self):
        completed = subprocess.run(
            [sys.executable, str(DRIVER), "--describe",
             "--offset-a", "12", "--offset-b", "16"],
            text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
        )

        self.assertNotEqual(completed.returncode, 0)
        self.assertIn("offset-a and offset-b must be coprime", completed.stdout)

    def test_scan_must_include_zero_pair_baseline(self):
        completed = subprocess.run(
            [sys.executable, str(DRIVER), "--describe",
             "--coarse-start", "100"],
            text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
        )

        self.assertNotEqual(completed.returncode, 0)
        self.assertIn("coarse-start must be zero", completed.stdout)

    def test_replacement_thresholds_cannot_overlap(self):
        completed = subprocess.run(
            [sys.executable, str(DRIVER), "--describe",
             "--old-max", "950", "--new-min", "900"],
            text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
        )

        self.assertNotEqual(completed.returncode, 0)
        self.assertIn("old-max must be smaller than new-min", completed.stdout)

    def test_describe_accepts_compact_training_mode(self):
        completed = subprocess.run(
            [sys.executable, str(DRIVER), "--describe", "--training-mode", "compact",
             "--coarse-stop", "200"],
            text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
        )

        self.assertEqual(completed.returncode, 0, completed.stdout)
        self.assertIn("training_mode=compact", completed.stdout)


if __name__ == "__main__":
    unittest.main()
