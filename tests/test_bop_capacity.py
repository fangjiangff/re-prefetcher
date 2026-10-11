import platform
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path


class BopCapacityCliTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.repo = Path(__file__).resolve().parent.parent
        cls.temporary_directory = tempfile.TemporaryDirectory()
        cls.binary = Path(cls.temporary_directory.name) / "test-bop-capacity"
        compiler = (
            "gcc"
            if platform.machine().lower() in ("aarch64", "arm64")
            else shutil.which("aarch64-linux-gnu-gcc")
        )
        if compiler is None:
            raise unittest.SkipTest("an AArch64 C compiler is required")
        cls.build_completed = subprocess.run(
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
                str(cls.repo / "re3-bop" / "test-bop-capacity.c"),
                str(cls.repo / "re0-stride-prefetcher" / "until.c"),
                "-o",
                str(cls.binary),
            ],
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
        )

    @classmethod
    def tearDownClass(cls):
        cls.temporary_directory.cleanup()

    def run_binary(self, *arguments):
        self.assertEqual(
            self.build_completed.returncode,
            0,
            f"capacity test build failed:\n{self.build_completed.stdout}",
        )
        if platform.machine().lower() not in ("aarch64", "arm64"):
            self.skipTest("running the AArch64 binary requires AArch64")
        return subprocess.run(
            [str(self.binary), *arguments],
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
        )

    def test_three_offsets_report_one_column_per_candidate(self):
        completed = self.run_binary(
            "--rounds",
            "1",
            "--train-pairs",
            "1",
            "--trigger-batch",
            "1",
            "--offsets",
            "4,8,16",
        )

        self.assertEqual(completed.returncode, 0, completed.stdout)
        lines = completed.stdout.splitlines()
        header = next(line for line in lines if line.startswith("candidate_count\t"))
        row = next(line for line in lines if line.startswith("3\t"))
        self.assertEqual(
            header,
            "candidate_count\tall_active_rounds\toffset_4_hits_per_1000\t"
            "offset_8_hits_per_1000\toffset_16_hits_per_1000",
        )
        self.assertEqual(len(row.split("\t")), 5)

    def test_duplicate_offsets_are_rejected(self):
        completed = self.run_binary("--offsets", "4,8,4")

        self.assertNotEqual(completed.returncode, 0)
        self.assertIn("offsets must be distinct", completed.stdout)


if __name__ == "__main__":
    unittest.main()
