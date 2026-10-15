from pathlib import Path
import runpy
import sys
import unittest
from unittest.mock import patch


SCRIPT = Path(__file__).resolve().parents[1] / "re0-stride-prefetcher/test0-trigger/test0-policy-old.py"


class PolicyProbeTests(unittest.TestCase):
    def load_script(self, position, *extra):
        with patch.object(sys, "argv", [str(SCRIPT), "--arch", "X925", "--rounds", "1",
                                        "--probe-pos", str(position), *extra]), \
                patch.object(sys, "path", [str(SCRIPT.parent), *sys.path]):
            return runpy.run_path(str(SCRIPT))

    def test_probe_position_reaches_compiler_and_output_name(self):
        for position in (1, 64, 99):
            with self.subTest(position=position):
                module = self.load_script(position)
                with patch("subprocess.run") as run:
                    module["compile_test"]("X925")
                    self.assertIn(f"-DPROBE_POS={position}", run.call_args.args[0])
                self.assertIn(f"-rounds1-probe{position}", module["result_name"]("X925", 6))

    def test_invalid_positions_are_rejected(self):
        for position in (-1, 100):
            with self.subTest(position=position), self.assertRaises(SystemExit) as error:
                self.load_script(position)
            self.assertEqual(error.exception.code, 2)

    def test_cpp_flag_reaches_compiler_and_output_name(self):
        for enabled in (False, True):
            module = self.load_script(13, *(("--cpp-rctx",) if enabled else ()))
            with patch("subprocess.run") as run:
                module["compile_test"]("X925")
                command = run.call_args.args[0]
                self.assertIn(f"-DENABLE_CPP_RCTX={int(enabled)}", command)
                self.assertEqual("-march=armv8.5-a+predres" in command, enabled)
            self.assertEqual(module["result_name"]("X925", 6).endswith("-cpp-rctx"), enabled)

    def test_cpp_rejects_unsupported_target(self):
        with self.assertRaises(SystemExit) as error:
            self.load_script(13, "--cpp-rctx", "--arch", "A53")
        self.assertEqual(error.exception.code, 2)
