"""KP920 CLI and native PMU/cache integration checks (python3 -m unittest)."""
import contextlib
import io
from pathlib import Path
import platform
import runpy
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

SCRIPT = Path(__file__).with_name("test0-exist.py")


def load_driver(*options):
    with patch.object(sys, "argv", [str(SCRIPT), "--arch", "KP920", *options]):
        return runpy.run_path(str(SCRIPT))


class KP920Tests(unittest.TestCase):
    def test_direct_pmu_timer_is_rejected(self):
        with contextlib.redirect_stderr(io.StringIO()):
            with self.assertRaises(SystemExit) as error:
                load_driver("--timer", "pmccntr")
            self.assertEqual(error.exception.code, 2)

    def test_pmu_defaults_and_overrides(self):
        driver = load_driver("--pmu-device", "armv8_pmuv3_0")
        self.assertEqual(driver["targets"](), [("KP920", 0)])
        self.assertEqual(driver["pmu_filter_values"]("KP920"), [(0, 1)])
        self.assertEqual(driver["timer_unit_for"]("KP920"), "ticks")
        self.assertEqual(driver["pmu_filter_values"]("A725"), [(1, 0), (0, 1)])
        driver = load_driver("--pmu-exclude-user", "1", "--pmu-exclude-kernel", "0")
        self.assertEqual(driver["pmu_filter_values"]("KP920"), [(1, 0)])
        driver = load_driver("--no-pmu")
        self.assertEqual(driver["pmu_filter_values"]("KP920"), [(0, 0)])

    def test_training_starts_at_line_three(self):
        driver = load_driver("--stride", "4", "--train-step", "7")
        self.assertEqual(driver["predicted_position"](7), 31)
        self.assertEqual(driver["single_probe_position"](7), 31)
        self.assertEqual(driver["accessed_offsets"](7),
                         {192, 448, 704, 960, 1216, 1472, 1728})
        self.assertEqual(driver["classify_position"](192, 99, 1, 7, 4), "accessed")
        self.assertEqual(driver["classify_position"](0, 99, 1, 7, 4), "cache_miss")

    @unittest.skipUnless(platform.machine() == "aarch64", "requires native AArch64")
    def test_native_single_probe_runs_without_perf_or_rctx(self):
        driver = load_driver("--probe-mode", "single", "--rounds", "20",
                             "--hit-threshold-ns", "4", "--no-pmu")
        driver["args"].current_pmu_exclude_user = 0
        driver["args"].current_pmu_exclude_kernel = 0
        with tempfile.TemporaryDirectory() as temp:
            # Override the executable destination only; compile and execute real code.
            driver["compile_test"].__globals__["OUT"] = str(Path(temp) / "probe")
            self.assertEqual(driver["compile_test"](3, "KP920"), 0)
            core = min(__import__("os").sched_getaffinity(0))
            result = subprocess.run(["taskset", "-c", str(core), str(Path(temp) / "probe")],
                                    text=True, capture_output=True, check=True)
            self.assertIn("ENABLE_CPP_RCTX=0", result.stdout)
            self.assertIn("# PMU disabled: cache probe only", result.stdout)
            rows = driver["parse_output"](result.stdout, 3, 4)
            self.assertEqual(len(rows), 1)
            self.assertEqual(rows[0]["position"], 18)
            self.assertEqual(rows[0]["probes"], 20)
            symbols = subprocess.check_output(["nm", str(Path(temp) / "probe")], text=True)
            self.assertNotIn("pmu_perf_event_open", symbols)
            self.assertNotIn("check_cpp_rctx_el0_access", symbols)

    @unittest.skipUnless(platform.machine() == "aarch64", "requires native AArch64")
    def test_native_pmu_pass_and_failure(self):
        driver = load_driver("--probe-mode", "single", "--rounds", "20",
                             "--hit-threshold-ns", "4")
        driver["args"].current_pmu_exclude_user = 0
        driver["args"].current_pmu_exclude_kernel = 1
        with tempfile.TemporaryDirectory() as temp:
            binary = str(Path(temp) / "probe")
            driver["compile_test"].__globals__["OUT"] = binary
            self.assertEqual(driver["compile_test"](0, "KP920"), 0)
            core = min(__import__("os").sched_getaffinity(0))
            command = ["taskset", "-c", str(core), binary]
            result = subprocess.run(command, text=True, capture_output=True, check=True)
            self.assertIn("core=KP920", result.stdout)
            self.assertIn("events=1/1", result.stdout)
            self.assertIn("separate-pass", result.stdout)
            self.assertRegex(result.stdout, r"# PMU PRF_REQ\s+event=0x6013 total=\d+ per_round=[0-9.]+ windows=20")
            rows = driver["parse_output"](result.stdout, 0, 4)
            self.assertEqual(rows[0]["probes"], 20)
            env = __import__("os").environ.copy()
            env["PMU_DEVICE"] = "nonexistent_test_pmu"
            result = subprocess.run(command, env=env, text=True, capture_output=True)
            self.assertNotEqual(result.returncode, 0)
            self.assertNotIn("per_round=", result.stdout)


if __name__ == "__main__":
    unittest.main()
