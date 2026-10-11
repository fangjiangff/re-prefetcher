import platform
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path


class BopCompetitionCliTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.repo = Path(__file__).resolve().parent.parent
        cls.temporary_directory = tempfile.TemporaryDirectory()
        cls.binary = Path(cls.temporary_directory.name) / "test-bop-competition"
        compiler = (
            "gcc"
            if platform.machine().lower() in ("aarch64", "arm64")
            else shutil.which("aarch64-linux-gnu-gcc")
        )
        if compiler is None:
            raise unittest.SkipTest("an AArch64 C compiler is required")

        command = [
            compiler,
            "-std=gnu11",
            "-O2",
            "-Wall",
            "-Wextra",
            "-Werror",
            "-Wno-unused-result",
            "-march=armv8.5-a+predres",
            "-DENABLE_CPP_RCTX=1",
            str(cls.repo / "re3-bop" / "test-bop-competition.c"),
            str(cls.repo / "re0-stride-prefetcher" / "until.c"),
            "-o",
            str(cls.binary),
        ]
        cls.build_completed = subprocess.run(
            command, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT
        )

    @classmethod
    def tearDownClass(cls):
        cls.temporary_directory.cleanup()

    def run_binary(self, *arguments):
        self.assertEqual(
            self.build_completed.returncode,
            0,
            f"competition test build failed:\n{self.build_completed.stdout}",
        )
        if platform.machine().lower() not in ("aarch64", "arm64"):
            self.skipTest("running the AArch64 binary requires AArch64")
        return subprocess.run(
            [str(self.binary), *arguments],
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
        )

    def test_describe_reports_balanced_order_and_dominance_controls(self):
        completed = self.run_binary("--describe")

        self.assertEqual(completed.returncode, 0, completed.stdout)
        self.assertEqual(
            completed.stdout.splitlines(),
            [
                "case\ta_weight\tb_weight\tfirst\taddress_stream",
                "untrained\t0\t0\tnone\tpaired",
                "only_a\t1\t0\ta\tpaired",
                "only_b\t0\t1\tb\tpaired",
                "balanced_a_first\t1\t1\ta\tpaired",
                "balanced_b_first\t1\t1\tb\tpaired",
                "a_dominant\t3\t1\ta\tpaired",
                "b_dominant\t1\t3\tb\tpaired",
            ],
        )

    def test_default_training_is_long_enough_for_competition_convergence(self):
        completed = self.run_binary("--help")

        self.assertEqual(completed.returncode, 0, completed.stdout)
        self.assertIn(
            "total pairs in every trained case (default 3000)", completed.stdout
        )

    def test_transition_arguments_require_both_phases_and_are_documented(self):
        completed = self.run_binary("--help")

        self.assertEqual(completed.returncode, 0, completed.stdout)
        self.assertIn("--transition-a-pairs N", completed.stdout)
        self.assertIn("--transition-b-pairs N", completed.stdout)

        incomplete = self.run_binary("--transition-a-pairs", "10", "--describe")
        self.assertNotEqual(incomplete.returncode, 0)
        self.assertIn("must be specified together", incomplete.stdout)

    def test_transition_mode_reports_a_sequential_training_row(self):
        completed = self.run_binary(
            "--rounds", "1", "--train-pairs", "1", "--trigger-batch", "1",
            "--transition-a-pairs", "4", "--transition-b-pairs", "6",
        )

        self.assertEqual(completed.returncode, 0, completed.stdout)
        lines = completed.stdout.splitlines()
        header_index = next(
            i for i, line in enumerate(lines) if line.startswith("case\t")
        )
        rows = [line.split("\t") for line in lines[header_index + 1 :]]
        transition = next(row for row in rows if row[0] == "transition_a_then_b")
        self.assertEqual(transition[1:3], ["4", "6"])
        self.assertEqual(len(rows), 8)

    def test_only_transition_accepts_zero_b_pairs_and_reports_seed(self):
        completed = self.run_binary(
            "--rounds", "1", "--train-pairs", "1", "--trigger-batch", "1",
            "--offset-a", "13", "--offset-b", "16",
            "--transition-a-pairs", "4", "--transition-b-pairs", "0",
            "--only-transition", "--seed", "123",
        )

        self.assertEqual(completed.returncode, 0, completed.stdout)
        self.assertIn("# seed=123", completed.stdout)
        lines = completed.stdout.splitlines()
        header_index = next(
            i for i, line in enumerate(lines) if line.startswith("case\t")
        )
        rows = [line.split("\t") for line in lines[header_index + 1 :]]
        self.assertEqual(len(rows), 1)
        self.assertEqual(rows[0][0:3], ["transition_a_then_b", "4", "0"])

    def test_isolated_probe_mode_measures_targets_in_separate_rounds(self):
        completed = self.run_binary(
            "--rounds", "2", "--train-pairs", "1", "--trigger-batch", "1",
            "--transition-a-pairs", "4", "--transition-b-pairs", "2",
            "--only-transition", "--isolated-probes",
        )

        self.assertEqual(completed.returncode, 0, completed.stdout)
        self.assertIn("# probe_mode=isolated", completed.stdout)
        self.assertIn("odd/even rounds probe only one offset", completed.stdout)
        lines = completed.stdout.splitlines()
        header_index = next(
            i for i, line in enumerate(lines) if line.startswith("case\t")
        )
        row = lines[header_index + 1].split("\t")
        self.assertTrue(all(int(value) == 0 for value in row[7:19]))

    def test_compact_training_mode_reports_selected_mode(self):
        completed = self.run_binary(
            "--rounds", "2", "--train-pairs", "1", "--trigger-batch", "1",
            "--transition-a-pairs", "4", "--transition-b-pairs", "2",
            "--only-transition", "--isolated-probes", "--training-mode", "compact",
        )

        self.assertEqual(completed.returncode, 0, completed.stdout)
        self.assertIn("# training_mode=compact", completed.stdout)

    def test_equal_offsets_are_rejected(self):
        completed = self.run_binary("--offset-a", "8", "--offset-b", "8")

        self.assertNotEqual(completed.returncode, 0)
        self.assertIn("offset-a and offset-b must be distinct", completed.stdout)

    def test_out_of_page_offset_is_rejected(self):
        completed = self.run_binary("--offset-b", "64")

        self.assertNotEqual(completed.returncode, 0)
        self.assertIn("offsets must be in cache lines 1..63", completed.stdout)

    def test_cpu_outside_affinity_mask_is_rejected(self):
        completed = self.run_binary("--cpu", "999999")

        self.assertNotEqual(completed.returncode, 0)
        self.assertIn("cpu must be smaller than", completed.stdout)

    def test_page_count_cannot_wrap_for_huge_training_value(self):
        completed = self.run_binary(
            "--train-pairs", "16140901064495857664", "--describe"
        )

        self.assertNotEqual(completed.returncode, 0)
        self.assertIn("mapped pool", completed.stdout)

    def test_training_and_noise_must_fit_before_trigger_pages(self):
        completed = self.run_binary(
            "--train-pairs", "8192", "--trigger-batch", "1", "--describe"
        )

        self.assertNotEqual(completed.returncode, 0)
        self.assertIn("mapped pool", completed.stdout)

    def test_result_reports_joint_activation_states(self):
        completed = self.run_binary(
            "--rounds", "1", "--train-pairs", "1", "--trigger-batch", "1"
        )

        self.assertEqual(completed.returncode, 0, completed.stdout)
        self.assertIn("BOP-inspired black-box", completed.stdout)
        self.assertIn(
            "all larger-offset targets are probed before any", completed.stdout
        )
        self.assertIn("round activity: active >=75%, inactive <=25%", completed.stdout)
        self.assertIn(
            "rounds_a_only\trounds_b_only\trounds_both\trounds_neither\t"
            "rounds_mixed",
            completed.stdout,
        )
        self.assertIn(
            "samples_a_only\tsamples_b_only\tsamples_both\tsamples_neither",
            completed.stdout,
        )

        lines = completed.stdout.splitlines()
        header_index = next(
            i for i, line in enumerate(lines) if line.startswith("case\t")
        )
        rows = [line.split("\t") for line in lines[header_index + 1 :]]
        self.assertEqual(len(rows), 7)
        self.assertTrue(all(len(row) == 21 for row in rows))
        for row in rows:
            train_total = int(row[1]) + int(row[2])
            self.assertEqual(train_total, 0 if row[0] == "untrained" else 1)
            self.assertEqual(sum(map(int, row[10:15])), 1)
            self.assertEqual(sum(map(int, row[15:19])), 1)


if __name__ == "__main__":
    unittest.main()
