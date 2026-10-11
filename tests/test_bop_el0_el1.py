import re
import os
import platform
import subprocess
import tempfile
import textwrap
import unittest
from pathlib import Path


class BopEl0El1ModuleTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.root = Path(__file__).resolve().parent.parent
        cls.module_dir = cls.root / "kernel-modules" / "bop-el0-el1"

    def test_module_interface_declares_trigger_ioctl_and_device(self):
        header = (self.module_dir / "bop_el0_el1_ioctl.h").read_text()

        self.assertIn('"/dev/bop_el0_el1"', header)
        self.assertRegex(header, r"BOP_EL0_EL1_IOC_TRIGGER")
        self.assertIn("struct bop_el0_el1_request", header)

    def test_module_has_separate_kernel_access_sequence(self):
        source = (self.module_dir / "bop_el0_el1.c").read_text()

        self.assertIn("get_user", source)
        self.assertIn("base_addr", source)
        self.assertIn("offset_lines", source)
        self.assertRegex(source, r"copy_to_user|put_user")

    def test_user_driver_contains_three_experiment_cases(self):
        source = (self.module_dir / "test_bop_el0_el1.c").read_text()

        for name in ("user_trigger", "kernel_trigger", "untrained_kernel",
                     "el0_only_user_trigger", "kernel_train_user_trigger",
                     "kernel_stride_user_trigger"):
            self.assertIn(name, source)

        self.assertIn("BOP_EL0_EL1_IOC_TRAIN", source)
        self.assertIn("BOP_EL0_EL1_TRAIN_STRIDE", source)
        module_source = (self.module_dir / "bop_el0_el1.c").read_text()
        self.assertIn("i * delta", module_source)
        self.assertIn("kernel_train_pairs", source)

    def test_user_driver_supports_running_one_case_per_process(self):
        source = (self.module_dir / "test_bop_el0_el1.c").read_text()

        self.assertIn("--case", source)
        self.assertIn("case_name", source)

    def test_probe_is_offset_from_the_kernel_trigger(self):
        source = (self.module_dir / "test_bop_el0_el1.c").read_text()

        self.assertIn("probe = trigger + offset_lines * LINE_BYTES", source)

    def test_user_driver_prefaults_the_mapping_before_training(self):
        source = (self.module_dir / "test_bop_el0_el1.c").read_text()

        self.assertIn("MAP_POPULATE", source)

    def test_training_page_order_has_no_repeated_small_inter_pair_delta(self):
        source = textwrap.dedent(r"""
            #include <stdint.h>
            #include <stdio.h>
            #include "bop_el0_el1_ioctl.h"

            int main(void) {
                unsigned char seen[BOP_EL0_EL1_TRAIN_PAGES] = {0};
                uint32_t previous = bop_el0_el1_pair_page(0, 1);
                seen[previous] = 1;
                for (uint32_t i = 1; i < 1400; i++) {
                    uint32_t page = bop_el0_el1_pair_page(i, 1);
                    long boundary_delta = ((long)page - previous) * 64 - 7;
                    if (page >= BOP_EL0_EL1_TRAIN_PAGES || seen[page])
                        return 1;
                    if (boundary_delta >= -63 && boundary_delta <= 63)
                        return 2;
                    seen[page] = 1;
                    previous = page;
                }
                printf("%u %u %u\n",
                       bop_el0_el1_pair_page(0, 1),
                       bop_el0_el1_pair_page(1, 1),
                       bop_el0_el1_pair_page(2, 1));
                return 0;
            }
        """)
        with tempfile.TemporaryDirectory() as directory:
            helper = Path(directory) / "order.c"
            binary = Path(directory) / "order"
            helper.write_text(source)
            build = subprocess.run(
                ["cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                 "-I", str(self.module_dir), str(helper), "-o", str(binary)],
                text=True, capture_output=True)
            self.assertEqual(build.returncode, 0, build.stderr)
            run = subprocess.run([str(binary)], text=True, capture_output=True)
            self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
            self.assertEqual(run.stdout, "131 1108 2085\n")

    def test_ioctl_request_carries_training_page_seed(self):
        source = textwrap.dedent(r"""
            #include <stdio.h>
            #include "bop_el0_el1_ioctl.h"
            int main(void) {
                struct bop_el0_el1_request request = {.page_seed = 37};
                printf("%u\n", request.page_seed);
                return 0;
            }
        """)
        with tempfile.TemporaryDirectory() as directory:
            helper = Path(directory) / "request.c"
            binary = Path(directory) / "request"
            helper.write_text(source)
            build = subprocess.run(
                ["cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                 "-I", str(self.module_dir), str(helper), "-o", str(binary)],
                text=True, capture_output=True)
            self.assertEqual(build.returncode, 0, build.stderr)
            run = subprocess.run([str(binary)], text=True, capture_output=True, check=True)
            self.assertEqual(run.stdout, "37\n")

    @unittest.skipUnless(
        platform.machine() in ("aarch64", "arm64") and
        os.environ.get("BOP_HARDWARE_TESTS") == "1",
        "opt-in A725 hardware test",
    )
    def test_a725_el0_1400_pairs_triggers_offset_7(self):
        subprocess.run(["make", "probe"], cwd=self.module_dir, check=True)
        run = subprocess.run(["sudo", "-n",
            str(self.module_dir / "test_bop_el0_el1"),
            "--case", "el0_only_user_trigger",
            "--cpu", "4", "--rounds", "30", "--train-pairs", "1400",
            "--offset-lines", "7", "--threshold-ns", "100",
        ], text=True, capture_output=True, check=True)
        row = next(line.split("\t") for line in run.stdout.splitlines()
                   if line.startswith("el0_only_user_trigger\t"))
        self.assertGreaterEqual(int(row[2]), 21, run.stdout)


if __name__ == "__main__":
    unittest.main()
