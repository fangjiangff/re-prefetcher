import importlib.util
import os
import platform
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from types import SimpleNamespace


class BopRetentionCliTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.repo = Path(__file__).resolve().parent.parent
        cls.source = cls.repo / "re3-bop" / "test-bop-retention.c"
        cls.driver = cls.repo / "re3-bop" / "test-bop-retention.py"
        cls.temporary_directory = tempfile.TemporaryDirectory()
        cls.binary = Path(cls.temporary_directory.name) / "test-bop-retention"
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
            "-DENABLE_PMU=0",
            "-DENABLE_CACHE_PROBE=1",
            str(cls.source),
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
            f"retention test build failed:\n{self.build_completed.stdout}",
        )
        if platform.machine().lower() not in ("aarch64", "arm64"):
            self.skipTest("running the AArch64 binary requires AArch64")
        return subprocess.run(
            [str(self.binary), *arguments],
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
        )

    def test_describe_exposes_non_reinforcing_phase_order(self):
        completed = self.run_binary("--describe")

        self.assertEqual(completed.returncode, 0, completed.stdout)
        self.assertEqual(
            completed.stdout.splitlines(),
            [
                "phase\tpurpose\tdemand_reads_of_predicted_targets",
                "train\tlearn the selected offset\tyes",
                "decay\tconsume unrelated triggers\tno",
                "endpoint\tmeasure retained prefetching\tafter all triggers",
            ],
        )

    def test_small_cache_run_reports_only_endpoint_probes(self):
        cpu = min(os.sched_getaffinity(0))
        completed = self.run_binary(
            "--cpu", str(cpu),
            "--repeats", "1",
            "--train-pairs", "1",
            "--decay-triggers", "3",
            "--probe-batch", "2",
            "--offset-lines", "16",
            "--threshold-ns", "80",
        )

        self.assertEqual(completed.returncode, 0, completed.stdout)
        self.assertIn("# decay targets are never demand-accessed", completed.stdout)
        lines = completed.stdout.splitlines()
        header_index = lines.index(
            "mode\toffset_lines\trepeats\ttrain_pairs\tdecay_triggers\t"
            "probe_batch\tprobes\tcache_hits\thits_per_1000\t"
            "avg_latency_ns\tl1_total\tl1_per_trigger\tl2_total\t"
            "l2_per_trigger"
        )
        row = lines[header_index + 1].split("\t")
        self.assertEqual(len(row), 14)
        self.assertEqual(row[:7], ["cache", "16", "1", "1", "3", "2", "2"])
        self.assertEqual(row[10:], ["-", "-", "-", "-"])

    def test_zero_probe_batch_is_rejected(self):
        completed = self.run_binary("--probe-batch", "0")

        self.assertNotEqual(completed.returncode, 0)
        self.assertIn("probe-batch must be positive", completed.stdout)

    def test_offset_outside_one_page_is_rejected(self):
        completed = self.run_binary("--offset-lines", "64")

        self.assertNotEqual(completed.returncode, 0)
        self.assertIn("offset-lines must be in 1..63", completed.stdout)

    def test_address_sets_must_fit_without_size_overflow(self):
        completed = self.run_binary(
            "--decay-triggers", "18446744073709551615"
        )

        self.assertNotEqual(completed.returncode, 0)
        self.assertIn("address sets exceed mapped pool", completed.stdout)

    def test_driver_describe_runs_all_pmu_points_before_cache_points(self):
        completed = subprocess.run(
            [
                sys.executable,
                str(self.driver),
                "--describe",
                "--decay-values", "0,64,256",
            ],
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
        )

        self.assertEqual(completed.returncode, 0, completed.stdout)
        self.assertEqual(
            completed.stdout.splitlines(),
            [
                "stage\tdecay_triggers",
                "pmu\t0",
                "pmu\t64",
                "pmu\t256",
                "cache\t0",
                "cache\t64",
                "cache\t256",
            ],
        )

    def test_default_result_name_encodes_configuration_that_changes_results(self):
        spec = importlib.util.spec_from_file_location("bop_retention_driver", self.driver)
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        args = SimpleNamespace(
            output=None,
            arch="X925",
            core=6,
            offset_lines=16,
            train_pairs=3000,
            repeats=20,
            probe_batch=64,
        )

        self.assertEqual(
            module.output_path(args).name,
            "bop-X925-cpu6-off16-train3000-repeats20-probe64-retention.tsv",
        )

    def test_default_result_path_never_overwrites_an_existing_run(self):
        spec = importlib.util.spec_from_file_location("bop_retention_unique", self.driver)
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        args = SimpleNamespace(
            output=None,
            arch="X925",
            core=6,
            offset_lines=16,
            train_pairs=3000,
            repeats=20,
            probe_batch=64,
        )
        with tempfile.TemporaryDirectory() as directory:
            module.RESULT_DIR = Path(directory)
            first = module.output_path(args)
            first.write_text("first run\n")

            self.assertEqual(
                module.output_path(args).name,
                "bop-X925-cpu6-off16-train3000-repeats20-probe64-"
                "retention-run2.tsv",
            )

    def test_result_file_records_complete_experiment_metadata(self):
        cpu = min(os.sched_getaffinity(0))
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / "retention.tsv"
            completed = subprocess.run(
                [
                    sys.executable,
                    str(self.driver),
                    "--stage", "cache",
                    "--core", str(cpu),
                    "--repeats", "1",
                    "--train-pairs", "1",
                    "--probe-batch", "2",
                    "--decay-values", "0",
                    "--output", str(output),
                ],
                text=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
            )

            self.assertEqual(completed.returncode, 0, completed.stdout)
            self.assertEqual(
                output.read_text().splitlines()[:3],
                [
                    "# BOP best-offset retention: PMU pass precedes cache pass",
                    f"# arch=X925 core={cpu} offset_lines=16 train_pairs=1 "
                    "repeats=1 probe_batch=2 threshold_ns=80 wait_nops=2000 "
                    "stages=cache",
                    "# decay_targets_demand_accessed=no",
                ],
            )


if __name__ == "__main__":
    unittest.main()
