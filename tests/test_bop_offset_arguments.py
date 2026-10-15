import contextlib
import io
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

from test_bop_runner import DRIVER, load_driver


class BopOffsetArgumentsTests(unittest.TestCase):
    def test_offset_reaches_binary_and_output_names(self):
        driver = load_driver()
        for options, offset in (([], 16), (["--offset", "16"], 16),
                                (["--offset", "6"], 6), (["--offset", "0"], 0)):
            for sweep in ([], ["--sweep-train-pairs"], ["--sweep-offsets"]):
                with self.subTest(options=options, sweep=sweep):
                    with mock.patch.object(sys, "argv", [str(DRIVER), "--type", "store", *options, *sweep]):
                        args = driver.parse_args()
                    self.assertEqual(driver.run_arguments(args),
                                     ["100", "80", "1024", "1", str(offset)])
                    args.pre_trigger_action = "yield"
                    self.assertEqual(driver.run_arguments(args),
                                     ["100", "80", "1024", "1", str(offset), "2"])
                    with tempfile.TemporaryDirectory() as directory, mock.patch.object(driver, "RESULT_DIR", Path(directory)):
                        for path in (*driver.output_paths(args), *driver.sweep_output_paths(args)):
                            self.assertIn(f"-off{offset}-", path.name)
                        self.assertIn("off0-128", driver.offset_sweep_output_path(args).name)
                        args.output = str(Path(directory) / "custom.tsv")
                        args.plot = str(Path(directory) / "custom.png")
                        self.assertEqual(driver.output_paths(args)[0], Path(args.output))
                        self.assertEqual(driver.sweep_output_paths(args), (Path(args.output), Path(args.plot)))

    def test_run_prints_offset_and_passes_it_to_subprocess(self):
        driver = load_driver()
        with mock.patch.object(sys, "argv", [str(DRIVER), "--sweep-train-pairs"]):
            args = driver.parse_args()
        output = io.StringIO()
        with mock.patch.object(driver.platform, "machine", return_value="aarch64"), mock.patch.object(driver.subprocess, "run") as run, contextlib.redirect_stdout(output):
            run.return_value = mock.Mock(returncode=0, stdout="")
            driver.run_binary(args, Path("/tmp/test-bop"), "PMU-only")
        self.assertEqual(run.call_args.args[0][-5:], ["100", "80", "1024", "1", "16"])
        self.assertIn("offset_lines=16", output.getvalue())


if __name__ == "__main__":
    unittest.main()
