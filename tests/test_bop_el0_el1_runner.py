import importlib.util
import tempfile
import unittest
from pathlib import Path


class BopEl0El1RunnerTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.path = (
            Path(__file__).resolve().parent.parent
            / "kernel-modules"
            / "bop-el0-el1"
            / "run_bop_el0_el1.py"
        )

    def load_module(self):
        spec = importlib.util.spec_from_file_location("bop_runner", self.path)
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        return module

    def test_runner_keeps_cases_as_separate_commands(self):
        runner = self.load_module()
        commands = runner.case_commands(
            binary="./test_bop_el0_el1",
            cpu=6,
            device="/dev/bop_el0_el1",
            rounds=3,
            train_pairs=10,
            offset_lines=16,
            threshold_ns=80,
            use_sudo=False,
        )

        self.assertEqual([command[command.index("--case") + 1]
                          for command in commands], [
                          "untrained_kernel",
                          "el0_only_user_trigger",
                          "kernel_trigger",
                          "kernel_train_user_trigger",
                          "kernel_stride_user_trigger",
                          ])
        self.assertEqual(len(commands), 5)

    def test_parser_has_output_option(self):
        runner = self.load_module()
        parser = runner.make_parser()

        options = {option for action in parser._actions for option in action.option_strings}
        self.assertIn("--output", options)

    def test_arch_selects_defaults_and_explicit_values_override_them(self):
        runner = self.load_module()

        x925 = runner.parse_args(["--arch", "X925"])
        a725 = runner.parse_args(["--arch", "A725"])
        overridden = runner.parse_args([
            "--arch", "A725", "--cpu", "12", "--threshold-ns", "77",
        ])

        self.assertEqual((x925.cpu, x925.threshold_ns), (6, 80))
        self.assertEqual((a725.cpu, a725.threshold_ns), (4, 100))
        self.assertEqual((overridden.cpu, overridden.threshold_ns), (12, 77))

    def test_midr_must_match_selected_arch(self):
        runner = self.load_module()

        runner.validate_midr("X925", 0x410FD850)
        runner.validate_midr("A725", 0x410FD870)
        with self.assertRaisesRegex(ValueError, "A725"):
            runner.validate_midr("A725", 0x410FD850)

    def test_summary_output_records_arch(self):
        runner = self.load_module()
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / "result.tsv"
            runner.write_summary(output, "A725", [["kernel_trigger", "30", "29", "966.67"]])

            self.assertEqual(output.read_text(),
                             "arch\tcase\tprobes\thits\thits_per_1000\n"
                             "A725\tkernel_trigger\t30\t29\t966.67\n")


if __name__ == "__main__":
    unittest.main()
