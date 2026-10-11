import csv
import io
import os
from pathlib import Path
import platform
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
HERE = ROOT / 're3-bop'


@unittest.skipUnless(platform.machine() == 'aarch64', 'AArch64 build')
class BopVmTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)

    def build(self):
        source = HERE / 'test-bop-vm.c'
        self.assertTrue(source.exists(), 'missing minimal KVM host')
        binary = Path(self.temp.name) / 'test-bop-vm'
        subprocess.run(['gcc', '-O2', '-Wall', '-Wextra', '-Werror', '-std=gnu11',
                        str(source), '-o', str(binary)], check=True)
        return binary

    def test_case_plan_and_probe_owner(self):
        binary = self.build()
        for case, train, probe, path in [
            ('untrained',0,'source','source'), ('local',1500,'source','source'),
            ('same_exit',1500,'source','source-source'),
            ('other_idle_return',1500,'source','source-peer-source'),
            ('cross_probe',1500,'peer','source-peer'),
            ('cross_no_trigger',1500,'peer','source-peer'),
            ('peer_local',1500,'peer','peer'), ('untrained_peer',0,'peer','peer')]:
            run = subprocess.run([str(binary),'--describe','--case',case],
                                 text=True,capture_output=True,check=True)
            row = dict(x.split('=') for x in run.stdout.split())
            self.assertEqual((int(row['train_pairs']),row['probe'],row['path']),
                             (train,probe,path))

    def test_reject_unsafe_bounds_before_kvm(self):
        binary = self.build()
        for option,value in [('--cpu','1024'),('--train-pairs','4097'),
                             ('--offset-lines','64'),('--rounds','0'),
                             ('--timeout','0'),('--direction','2'),('--case','bad')]:
            run = subprocess.run([str(binary),'--describe',option,value],capture_output=True)
            self.assertEqual(run.returncode,2,(option,value))

    @unittest.skipUnless(os.environ.get('BOP_KVM_TESTS') == '1','opt-in KVM execution')
    def test_two_guests_smoke_and_cache_configuration(self):
        binary = self.build()
        subprocess.run(['make','-C',str(HERE/'vm-bop')],check=True)
        run = subprocess.run(['sudo','-n',str(binary),'--guest',str(HERE/'vm-bop/guest.bin'),
                              '--smoke','--timeout','5'],check=True,text=True,capture_output=True,timeout=12)
        row, = csv.DictReader(io.StringIO(run.stdout),delimiter='\t')
        self.assertEqual(row['status'],'smoke_ok')
        self.assertEqual((row['guest_el_a'],row['guest_el_b']),('1','1'))
        self.assertEqual((row['cache_a'],row['cache_b']),('1','1'))
        self.assertEqual(row['vms'],'2')


if __name__ == '__main__':
    unittest.main()
