from pathlib import Path
import platform
import re
import subprocess
import tempfile
import unittest


@unittest.skipUnless(platform.machine() == "aarch64", "AArch64 disassembly")
class PolicyPCsTests(unittest.TestCase):
    def test_training_instructions_have_distinct_pcs(self):
        source = Path(__file__).resolve().parents[1] / "re0-stride-prefetcher/test0-trigger"
        with tempfile.TemporaryDirectory() as directory:
            binary = Path(directory) / "policy"
            for mode, instruction in (("STORE", "strb"), ("LOAD", "ldrb"),
                                      ("PREFETCH", "prfm")):
                with self.subTest(mode=mode):
                    subprocess.run([
                        "gcc", "-O0", f"-DTRAIN_ACCESS_{mode}=1",
                        str(source / "test0-policy-old.c"),
                        str(source / "until.c"), "-o", str(binary),
                    ], check=True)
                    dump = subprocess.check_output(["objdump", "-d", str(binary)], text=True)
                    pcs = []
                    for index in range(16):
                        body = re.search(rf"<train_pc{index}>:\n(.*?)(?=\n\n)",
                                         dump, re.S)
                        self.assertIsNotNone(body, f"missing train_pc{index}")
                        accesses = re.findall(rf"^\s*([0-9a-f]+):.*\b{instruction}\b",
                                              body[1], re.M)
                        self.assertEqual(len(accesses), 1)
                        pcs.extend(accesses)
                    self.assertEqual(len(set(pcs)), 16)
