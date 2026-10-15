import importlib.util
import unittest
from pathlib import Path


class BopLeftOffsetTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.root = Path(__file__).resolve().parent.parent
        cls.src = cls.root / "re3-bop" / "test-bop-left-offset.c"
        cls.script = cls.root / "re3-bop" / "test-bop-left-offset.py"

    def test_c_defines_all_left_training_modes(self):
        source = self.src.read_text()
        for mode in ("none", "single_pc_stride", "multi_pc_stride",
                     "single_pc_page_pair", "multi_page_pair"):
            self.assertIn(mode, source)
        self.assertIn("train_left_offset", source)
        self.assertIn("offset_lines", source)

    def test_runner_has_independent_mode_commands(self):
        spec = importlib.util.spec_from_file_location("left_runner", self.script)
        runner = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(runner)
        commands = runner.mode_commands("./test", 6, 16, 100, 200, 30, 64)

        self.assertEqual(len(commands), 5)
        self.assertEqual([command[command.index("--mode") + 1]
                          for command in commands], [
                              "none", "single_pc_stride", "multi_pc_stride",
                              "single_pc_page_pair", "multi_page_pair",
                          ])


if __name__ == "__main__":
    unittest.main()
