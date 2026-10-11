import importlib.util
import os
import platform
import re
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from types import SimpleNamespace
from unittest import mock


REPO = Path(__file__).resolve().parent.parent
DRIVER = REPO / "re3-bop" / "test-bop.py"


def load_driver():
    spec = importlib.util.spec_from_file_location("test_bop_runner", DRIVER)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


class BopRunnerPmuBindingTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.driver = load_driver()

    def test_loaded_module_on_another_cpu_is_unloaded_and_rebound(self):
        state = {"loaded": True, "target_cpu": 6}
        calls = []

        def fake_is_dir(path):
            if str(path) == "/sys/module/prefetch_pmu_event":
                return state["loaded"]
            return False

        def fake_read_text(path):
            if str(path).endswith("/parameters/target_cpu"):
                return str(state["target_cpu"])
            if str(path) == "/proc/sys/kernel/perf_user_access":
                return "1"
            raise AssertionError(f"unexpected read: {path}")

        def fake_is_file(path):
            return str(path) == "/proc/sys/kernel/perf_user_access"

        def fake_module_script(name, *arguments):
            calls.append((name, *arguments))
            if name == "module_unload.sh":
                state["loaded"] = False
            elif name == "module_load.sh":
                state["loaded"] = True
                state["target_cpu"] = arguments[-1]

        args = SimpleNamespace(core=4)
        with (
            mock.patch.object(Path, "is_dir", fake_is_dir),
            mock.patch.object(Path, "is_file", fake_is_file),
            mock.patch.object(Path, "read_text", fake_read_text),
            mock.patch.object(
                self.driver, "module_script", side_effect=fake_module_script
            ),
        ):
            loaded_here = self.driver.load_pmu_module(args)

        self.assertTrue(loaded_here)
        self.assertEqual(
            calls,
            [
                ("module_unload.sh",),
                ("module_load.sh", "--cpu", 4),
            ],
        )

    def test_train_pair_sweep_starts_with_zero(self):
        self.assertEqual(self.driver.TRAIN_PAIR_SWEEP[0], 0)
        self.assertEqual(
            self.driver.TRAIN_PAIR_SWEEP[1:], tuple(range(100, 3001, 100))
        )

    def test_single_run_accepts_zero_training_pairs(self):
        with mock.patch.object(
            sys, "argv", [str(DRIVER), "--train-pairs", "0", "--no-run"]
        ):
            args = self.driver.parse_args()

        self.assertEqual(args.train_pairs, 0)

    def test_default_trigger_batch_is_one_position(self):
        with mock.patch.object(sys, "argv", [str(DRIVER), "--no-run"]):
            args = self.driver.parse_args()

        self.assertEqual(args.trigger_batch, 1)

    def test_a725_default_hit_threshold_is_100_ns(self):
        with mock.patch.object(
            sys, "argv", [str(DRIVER), "--arch", "A725", "--no-run"]
        ):
            args = self.driver.parse_args()

        self.assertEqual(args.threshold_ns, 100)

    def test_strict_case_order_includes_wrong_target_control(self):
        self.assertEqual(
            self.driver.CASE_ORDER,
            [
                "untrained",
                "random_delta_control",
                "fixed_offset_expected",
                "trained_wrong_target",
            ],
        )

    def test_offset_sweep_keeps_controls_and_marks_offset_zero_degenerate(self):
        cache = {
            case: {
                "hits_per_1000": index * 10,
                "avg_latency_ns": 100.0 + index,
            }
            for index, case in enumerate(self.driver.CASE_ORDER)
        }
        pmu = {
            case: {
                "l1d_cache_hwprf": {"per_trigger": index / 10},
                "l2d_cache_hwprf": {"per_trigger": index / 5},
            }
            for index, case in enumerate(self.driver.CASE_ORDER)
        }

        zero_row = self.driver.offset_sweep_row(0, pmu, cache)
        regular_row = self.driver.offset_sweep_row(1, pmu, cache)
        pmu_row = self.driver.pmu_average_row(1, pmu)
        zero_pmu_row = self.driver.pmu_average_row(0, pmu)

        self.assertEqual(zero_row[1], "-")
        self.assertEqual(zero_row[-1], "degenerate_same_address")
        self.assertEqual(regular_row[-1], "measurable")
        self.assertIn(
            str(cache["random_delta_control"]["hits_per_1000"]), regular_row
        )
        self.assertIn(
            str(cache["trained_wrong_target"]["hits_per_1000"]), regular_row
        )
        self.assertIn("0.100000", pmu_row)
        self.assertIn("0.600000", pmu_row)
        self.assertIn("0.600000", regular_row)
        self.assertEqual(zero_pmu_row[-1], "degenerate_same_address")

    def test_random_control_matches_fixed_mate_page_and_line_marginal(self):
        source = (REPO / "re3-bop" / "test-bop.c").read_text()

        self.assertIn("random_base_line", source)
        self.assertIn("mate_line = random_base_line + offset_lines", source)
        self.assertNotIn("span_pages * LINES_PER_PAGE", source)


class BopZeroTrainingTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler = (
            "gcc"
            if platform.machine().lower() in ("aarch64", "arm64")
            else shutil.which("aarch64-linux-gnu-gcc")
        )
        if compiler is None:
            raise unittest.SkipTest("an AArch64 C compiler is required")
        cls.temporary_directory = tempfile.TemporaryDirectory()
        cls.binary = Path(cls.temporary_directory.name) / "test-bop-cache"
        cls.build = subprocess.run(
            [
                compiler,
                "-std=gnu11",
                "-O2",
                "-Wall",
                "-Wextra",
                "-Werror",
                "-Wno-unused-result",
                "-march=armv8.5-a+predres",
                "-DENABLE_CPP_RCTX=1",
                "-DENABLE_PMU=0",
                "-DENABLE_CACHE_PROBE=1",
                str(REPO / "re3-bop" / "test-bop.c"),
                str(REPO / "re0-stride-prefetcher" / "until.c"),
                "-o",
                str(cls.binary),
            ],
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
        )

    @classmethod
    def tearDownClass(cls):
        if hasattr(cls, "temporary_directory"):
            cls.temporary_directory.cleanup()

    def test_zero_training_pairs_runs_as_a_baseline(self):
        self.assertEqual(self.build.returncode, 0, self.build.stdout)
        if platform.machine().lower() not in ("aarch64", "arm64"):
            self.skipTest("running the AArch64 binary requires AArch64")
        cpu = min(os.sched_getaffinity(0))
        completed = subprocess.run(
            [
                "taskset",
                "-c",
                str(cpu),
                str(self.binary),
                "1",
                "120",
                "0",
                "1",
            ],
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
        )

        self.assertEqual(completed.returncode, 0, completed.stdout)
        self.assertIn("# rounds=1 train_pairs=0", completed.stdout)
        self.assertIn("trigger_batch=1", completed.stdout)

    def run_cache_binary(self, train_pairs, offset="1"):
        self.assertEqual(self.build.returncode, 0, self.build.stdout)
        if platform.machine().lower() not in ("aarch64", "arm64"):
            self.skipTest("running the AArch64 binary requires AArch64")
        cpu = min(os.sched_getaffinity(0))
        return subprocess.run(
            [
                "taskset",
                "-c",
                str(cpu),
                str(self.binary),
                "1",
                "120",
                str(train_pairs),
                "1",
                offset,
            ],
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
        )

    def test_strict_training_metadata_and_wrong_target_control(self):
        completed = self.run_cache_binary(1)

        self.assertEqual(completed.returncode, 0, completed.stdout)
        self.assertIn(
            "# strict_training=1 training_flush=each "
            "training_noise_loads=0 trigger_slot_policy=fixed_tail",
            completed.stdout,
        )
        self.assertIn("[trained_wrong_target]", completed.stdout)
        self.assertIn("[random_delta_control]", completed.stdout)
        self.assertIn(
            "# train_mode=2 train_offset_lines=- "
            "probe_delta_lines=1 rounds=1",
            completed.stdout,
        )
        self.assertIn(
            "# train_mode=1 train_offset_lines=1 "
            "probe_delta_lines=2 rounds=1",
            completed.stdout,
        )

    def test_trigger_slot_is_independent_of_training_pair_count(self):
        zero = self.run_cache_binary(0)
        eight = self.run_cache_binary(8)

        self.assertEqual(zero.returncode, 0, zero.stdout)
        self.assertEqual(eight.returncode, 0, eight.stdout)
        pattern = re.compile(r"trigger_slot_first=(\d+)")
        self.assertEqual(
            pattern.search(zero.stdout).group(1),
            pattern.search(eight.stdout).group(1),
        )

    def test_singleton_random_control_is_marked_partially_matched(self):
        completed = self.run_cache_binary(1, offset="63")

        self.assertEqual(completed.returncode, 0, completed.stdout)
        self.assertIn(
            "# random_control_match=partial_singleton_line_range",
            completed.stdout,
        )


if __name__ == "__main__":
    unittest.main()
