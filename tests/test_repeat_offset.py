import contextlib
import importlib.util
import io
import platform
import re
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock


ROOT = Path(__file__).resolve().parents[1]
RUNNER = ROOT / "re3-bop" / "test-repeat-offset.py"


def load_runner():
    spec = importlib.util.spec_from_file_location("repeat_offset_runner", RUNNER)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


class RepeatOffsetBuildTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.compiler = (
            "gcc"
            if platform.machine().lower() in ("aarch64", "arm64")
            else shutil.which("aarch64-linux-gnu-gcc")
        )
        cls.objdump = (
            "objdump"
            if platform.machine().lower() in ("aarch64", "arm64")
            else shutil.which("aarch64-linux-gnu-objdump")
        )
        if cls.compiler is None or cls.objdump is None:
            raise unittest.SkipTest("an AArch64 compiler and objdump are required")

    def build(self, source):
        with tempfile.TemporaryDirectory() as directory:
            binary = Path(directory) / source.stem
            completed = subprocess.run(
                [
                    self.compiler,
                    "-std=gnu11",
                    "-O2",
                    "-Wall",
                    "-Wextra",
                    "-Wno-unused-result",
                    "-march=armv8.5-a+predres",
                    "-DENABLE_CPP_RCTX=1",
                    "-DENABLE_PMU=0",
                    "-DENABLE_CACHE_PROBE=1",
                    str(source),
                    str(ROOT / "re0-stride-prefetcher" / "until.c"),
                    "-o",
                    str(binary),
                ],
                text=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
            )
            self.assertEqual(completed.returncode, 0, completed.stdout)
            disassembly = subprocess.run(
                [self.objdump, "-d", str(binary)],
                check=True,
                text=True,
                stdout=subprocess.PIPE,
            ).stdout
            strings = subprocess.run(
                ["strings", str(binary)],
                check=True,
                text=True,
                stdout=subprocess.PIPE,
            ).stdout
            return disassembly, strings

    def test_repeat_pair_keeps_two_loads_in_one_function(self):
        disassembly, _ = self.build(ROOT / "re3-bop" / "test-repeat-offset.c")
        match = re.search(
            r"<repeat_offset_pair>:\n(?P<body>.*?)(?=\n\n)",
            disassembly,
            re.DOTALL,
        )

        self.assertIsNotNone(match, disassembly)
        memory_instructions = re.findall(
            r"\b(?:ldr|str|ldp|stp)\b[^\n]*", match.group("body")
        )
        self.assertEqual(len(memory_instructions), 2)
        self.assertRegex(memory_instructions[0], r"^ldr\tx1, \[x\d+\]$")
        self.assertRegex(memory_instructions[1], r"^ldr\tx2, \[x\d+\]$")
        self.assertEqual(match.group("body").count("dc\tcivac"), 2)

    def test_repeat_training_reports_fixed_flushed_addresses(self):
        _, strings = self.build(ROOT / "re3-bop" / "test-repeat-offset.c")

        self.assertIn("fixed_same_page_pair", strings)
        self.assertIn("each_pair_dc_civac", strings)

    def test_original_bop_build_still_succeeds(self):
        self.build(ROOT / "re3-bop" / "test-bop.c")


class RepeatOffsetRunnerTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.runner = load_runner()

    def test_build_only_uses_repeat_source_and_binary_name(self):
        compiler = (
            "gcc"
            if platform.machine().lower() in ("aarch64", "arm64")
            else shutil.which("aarch64-linux-gnu-gcc")
        )
        if compiler is None:
            self.skipTest("an AArch64 compiler is required")

        with tempfile.TemporaryDirectory() as directory:
            with (
                mock.patch.object(self.runner.driver, "BIN_DIR", Path(directory)),
                mock.patch.object(
                    sys,
                    "argv",
                    [
                        str(RUNNER),
                        "--cc",
                        compiler,
                        "--no-pmu",
                        "--no-run",
                    ],
                ),
                contextlib.redirect_stdout(io.StringIO()) as output,
            ):
                result = self.runner.driver.main()

            binary = Path(directory) / "test-repeat-offset-cache"
            self.assertEqual(result, 0)
            self.assertTrue(binary.is_file())
            self.assertIn(str(binary), output.getvalue())

    def test_store_training_is_rejected(self):
        with (
            mock.patch.object(
                sys, "argv", [str(RUNNER), "--type", "store", "--no-run"]
            ),
            mock.patch("sys.stderr"),
            self.assertRaises(SystemExit) as error,
        ):
            self.runner.driver.parse_args()

        self.assertEqual(error.exception.code, 2)

    def test_cross_page_offset_is_rejected(self):
        with (
            mock.patch.object(sys, "argv", [str(RUNNER), "--offset", "64"]),
            mock.patch("sys.stderr"),
            self.assertRaises(SystemExit) as error,
        ):
            self.runner.driver.parse_args()

        self.assertEqual(error.exception.code, 2)

    def test_negative_cross_page_offset_is_rejected(self):
        with contextlib.redirect_stderr(io.StringIO()) as error_output:
            with (
                mock.patch.object(sys, "argv", [str(RUNNER), "--offset", "-64"]),
                self.assertRaises(SystemExit) as error,
            ):
                self.runner.driver.parse_args()

        self.assertEqual(error.exception.code, 2)
        self.assertIn("absolute offset", error_output.getvalue())

    def test_negative_offset_runs_within_one_page(self):
        if platform.machine().lower() not in ("aarch64", "arm64"):
            self.skipTest("execution requires an AArch64 host")

        with tempfile.TemporaryDirectory() as directory:
            with (
                mock.patch.object(self.runner.driver, "BIN_DIR", Path(directory)),
                mock.patch.object(self.runner.driver, "RESULT_DIR", Path(directory)),
                mock.patch.object(
                    sys,
                    "argv",
                    [
                        str(RUNNER),
                        "--core",
                        "6",
                        "--rounds",
                        "1",
                        "--train-pairs",
                        "1",
                        "--offset",
                        "-16",
                        "--no-pmu",
                        "--no-plot",
                    ],
                ),
                contextlib.redirect_stdout(io.StringIO()) as output,
            ):
                result = self.runner.driver.main()

        self.assertEqual(result, 0)
        self.assertIn("# offset_lines=-16 offset_bytes=-1024", output.getvalue())

    def test_default_results_have_repeat_offset_prefix(self):
        with tempfile.TemporaryDirectory() as directory:
            with (
                mock.patch.object(
                    self.runner.driver, "RESULT_DIR", Path(directory)
                ),
                mock.patch.object(sys, "argv", [str(RUNNER), "--no-run"]),
            ):
                args = self.runner.driver.parse_args()
                outputs = (
                    self.runner.driver.output_paths(args)[0],
                    self.runner.driver.sweep_output_paths(args)[0],
                    self.runner.driver.offset_sweep_output_path(args),
                )

        for output in outputs:
            self.assertTrue(output.name.startswith("repeat-offset-"), output)
