import csv
import io
import importlib.util
import os
from pathlib import Path
import platform
import re
import subprocess
import tempfile
import unittest
from types import SimpleNamespace

ROOT = Path(__file__).resolve().parents[1]


def load_runner():
    path = ROOT / 're3-bop/test-bop-cross-context.py'
    spec = importlib.util.spec_from_file_location('context_runner', path)
    module = importlib.util.module_from_spec(spec); spec.loader.exec_module(module)
    return module


class CrossContextRunnerTests(unittest.TestCase):
    def test_faulted_measurement_is_rejected(self):
        module = load_runner()
        self.assertTrue(hasattr(module, 'validate_faults'), 'missing fault rejection')
        module.validate_faults(dict(critical_faults_a='0', critical_faults_b='0'))
        for a, b in [('1', '0'), ('0', '1')]:
            with self.assertRaisesRegex(RuntimeError, 'fault'):
                module.validate_faults(dict(critical_faults_a=a, critical_faults_b=b))

    def test_aggregation_keeps_context_and_pmu_passes_separate(self):
        path = ROOT / 're3-bop/test-bop-cross-context.py'
        self.assertTrue(path.exists(), 'missing runner')
        module = load_runner()
        rows = []
        for arch, mode, measure, hits, probes in [('X925', 'thread', 'cache', 10, 100),
                                                 ('X925', 'thread', 'cache', 30, 100),
                                                 ('X925', 'process', 'cache', 0, 100),
                                                 ('X925', 'thread', 'demand', 0, 0),
                                                 ('A725', 'thread', 'cache', 5, 100)]:
            row = dict(arch=arch, mode=mode, case='split_same', measure=measure, cpu_a='6',
                       n='300', m='300', full='1500', offset='16', probe_batch='64', hits=str(hits),
                       probes=str(probes), hit_pct=str(hits), critical_faults_a='0',
                       critical_faults_b='0')
            for key in ('avg_ns', 'a_event0_per_load', 'a_event1_per_load',
                        'b_event0_per_load', 'b_event1_per_load', 'handoff_us'):
                row[key] = '1.0'
            rows.append(row)
        result = module.aggregate(rows)
        self.assertEqual(len(result), 4)
        row, = [r for r in result if r['arch']=='X925' and
                r['mode']=='thread' and r['measure']=='cache']
        self.assertEqual((row['repeats'], row['probes'], row['hits']), (2, 200, 40))
        self.assertEqual((row['hit_pct'], row['hit_pct_min'], row['hit_pct_max']), (20, 10, 30))

    def test_arch_selects_defaults_and_allows_overrides(self):
        module = load_runner()
        a725 = module.parse_args(['--arch', 'A725'])
        self.assertEqual((a725.cpu, a725.threshold_ns), (4, 100))
        overridden = module.parse_args([
            '--arch', 'A725', '--cpu', '12', '--threshold-ns', '77'])
        self.assertEqual((overridden.cpu, overridden.threshold_ns), (12, 77))

    def test_probe_batch_defaults_to_64_and_accepts_one(self):
        module = load_runner()
        self.assertEqual(module.parse_args([]).probe_batch, 64)
        self.assertEqual(module.parse_args(['--probe-batch', '1']).probe_batch, 1)
        for value in ('0', '65'):
            with self.assertRaises(SystemExit):
                module.parse_args(['--probe-batch', value])

    def test_scrub_option_is_removed(self):
        module = load_runner()
        self.assertFalse(hasattr(module.parse_args([]), 'scrub_loads'))
        with self.assertRaises(SystemExit):
            module.parse_args(['--scrub-loads', '1'])

    def test_cross_context_uses_one_training_pool_without_scrub(self):
        source = (ROOT / 're3-bop/bop-cross-context.h').read_text()
        self.assertNotIn('a_second', source)
        self.assertNotIn('cc_scrub', source)
        self.assertIn('(CC_PAGES+CC_PROBE_PAGES)*PAGE_SIZE', source)

    def test_single_pool_rejects_local_nm_larger_than_4096_pairs(self):
        module = load_runner()
        with self.assertRaises(SystemExit):
            module.parse_args(['--train-a-pairs', '3000', '--train-b-pairs', '2000',
                               '--cases', 'local_nm'])

    def test_midr_must_match_selected_arch(self):
        module = load_runner()
        module.validate_midr('X925', 0xd850)
        module.validate_midr('A725', 0xd870)
        with self.assertRaisesRegex(ValueError, 'A725'):
            module.validate_midr('A725', 0xd850)

    def test_a725_build_uses_its_l2_prefetch_event(self):
        module = load_runner()
        command = module.build_command(Path('/tmp/cross'), Path('/tmp/cross.c'), 'A725')
        self.assertIn('-DCC_L2_PREFETCH_EVENT=0x8285', command)
        x925 = module.build_command(Path('/tmp/cross'), Path('/tmp/cross.c'), 'X925')
        self.assertIn('-DCC_L2_PREFETCH_EVENT=0x8155', x925)

    def test_pmu_measure_rejects_loaded_kernel_module(self):
        module = load_runner()
        module.validate_measure_environment(['cache'], True)
        with self.assertRaisesRegex(ValueError, 'unload'):
            module.validate_measure_environment(['prefetch', 'cache'], True)

    def test_default_output_uses_arch_and_avoids_existing_results(self):
        module = load_runner()
        args = SimpleNamespace(arch='A725', cpu=4, offset_lines=16, output=None)
        with tempfile.TemporaryDirectory() as directory:
            result_dir = Path(directory)
            first = module.choose_output_path(args, result_dir)
            self.assertEqual(first, result_dir / 'bop-A725-cpu4-off16-cross-context.tsv')
            first.touch()
            second = module.choose_output_path(args, result_dir)
            self.assertEqual(second, result_dir / 'bop-A725-cpu4-off16-cross-context-run2.tsv')

    def test_explicit_output_still_refuses_overwrite(self):
        module = load_runner()
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / 'chosen.tsv'
            output.touch()
            args = SimpleNamespace(arch='X925', cpu=6, offset_lines=16, output=output)
            with self.assertRaisesRegex(FileExistsError, 'chosen.tsv'):
                module.choose_output_path(args, Path(directory))


@unittest.skipUnless(platform.machine() in ('aarch64', 'arm64'), 'AArch64 experiment')
class CrossContextTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)

    def build(self, mode, arch='X925'):
        source = ROOT / f're3-bop/test-bop-cross-{mode}.c'
        self.assertTrue(source.exists(), f'missing {mode} experiment')
        binary = Path(self.temp.name) / mode
        command = load_runner().build_command(binary, source, arch)
        subprocess.run(command, check=True)
        return binary

    def test_describe_reports_arch_specific_l2_prefetch_event(self):
        for arch, event in [('X925', '0x8155'), ('A725', '0x8285')]:
            binary = self.build('thread', arch)
            run = subprocess.run([str(binary), '--describe'], capture_output=True,
                                 text=True, check=True)
            row = dict(item.split('=') for item in run.stdout.split())
            self.assertEqual(row['l2_prefetch_event'], event)

    def test_describe_accepts_single_probe_batch(self):
        binary = self.build('thread')
        run = subprocess.run(
            [str(binary), '--describe', '--probe-batch', '1'],
            capture_output=True, text=True, check=True)
        row = dict(item.split('=') for item in run.stdout.split())
        self.assertEqual(row['probe_batch'], '1')

    def test_training_flushes_both_addresses_before_either_load(self):
        binary = self.build('thread')
        disassembly = subprocess.run(
            ['objdump', '-d', str(binary)], capture_output=True,
            text=True, check=True).stdout
        match = re.search(r'<cc_train(?:\.part\.\d+)?>:\n(.*?)(?=\n\n)',
                          disassembly, re.DOTALL)
        self.assertIsNotNone(match, 'missing compiled cc_train function')
        operations = []
        for line in match.group(1).splitlines():
            if '\tdc\tcivac' in line:
                operations.append('flush')
            elif re.search(r'\bbl\b.*<cc_train_load>', line):
                operations.append('load')
        self.assertEqual(operations[:4], ['flush', 'flush', 'load', 'load'])

    def test_training_and_probe_ownership(self):
        binary = self.build('thread')
        # Wrong case dispatch, training amount or probe owner must fail these literals.
        cases = [('untrained', 0, 0, 'a', 1), ('local_n', 300, 0, 'a', 0),
                 ('local_nm', 600, 0, 'a', 0), ('switch_none', 300, 0, 'a', 1),
                 ('switch_local_nm', 600, 0, 'a', 1), ('split_same', 300, 300, 'a', 1),
                 ('split_other', 300, 300, 'a', 1), ('b_only', 0, 300, 'a', 1),
                 ('no_trigger', 300, 300, 'a', 1), ('retain_local', 1500, 0, 'a', 0),
                 ('retain_switch', 1500, 0, 'a', 1), ('retain_peer', 1500, 0, 'b', 1),
                 ('untrained_peer', 0, 0, 'b', 1), ('peer_local', 0, 1500, 'b', 1),
                 ('split_peer', 300, 300, 'b', 1)]
        for case, a, b, probe, switch in cases:
            run = subprocess.run([str(binary), '--describe', '--case', case],
                                 capture_output=True, text=True, check=True)
            row = dict(x.split('=') for x in run.stdout.split())
            self.assertEqual((int(row['a_pairs']), int(row['b_pairs']), row['probe'],
                              int(row['switch'])), (a, b, probe, switch), case)
            self.assertEqual(row['b_offset'], '13' if case == 'split_other' else '16')

    def test_invalid_parameters_rejected(self):
        binary = self.build('process')
        for args in [('--cpu', '1024'), ('--train-a-pairs', '4097'), ('--rounds', '0'),
                     ('--offset-lines', '64'), ('--other-offset', '16'),
                     ('--case', 'unknown'), ('--full-pairs', '-1')]:
            run = subprocess.run([str(binary), '--describe', *args], capture_output=True)
            self.assertEqual(run.returncode, 2, args)

    @unittest.skipUnless(os.environ.get('BOP_HARDWARE_TESTS') == '1', 'opt-in X925 CPU6 hardware test')
    def test_real_context_identity_and_probe_counts(self):
        # Forgetting fork/pthread or probing in the wrong owner must fail.
        for mode in ('thread', 'process'):
            binary = self.build(mode)
            for case, owner in [('untrained', 'a'), ('untrained_peer', 'b')]:
                run = subprocess.run([str(binary), '--cpu', '6', '--case', case,
                                      '--rounds', '2'],
                                     capture_output=True, text=True, check=True, timeout=15)
                row, = csv.DictReader(io.StringIO(run.stdout), delimiter='\t')
                self.assertEqual(row['mode'], mode)
                self.assertEqual(row['cpu_a'], '6'); self.assertEqual(row['cpu_b'], '6')
                self.assertNotEqual(row['tid_a'], row['tid_b'])
                self.assertEqual(row['pid_a'] == row['pid_b'], mode == 'thread')
                self.assertEqual(row['probe'], owner)
                self.assertEqual(int(row['probes']), 128)
                self.assertEqual(int(row['a_loads']) + int(row['b_loads']), 0)
                self.assertEqual(int(row['critical_faults_a']) + int(row['critical_faults_b']), 0)

    @unittest.skipUnless(os.environ.get('BOP_HARDWARE_TESTS') == '1', 'opt-in X925 CPU6 hardware test')
    def test_process_fault_accounting_does_not_fault_on_fork_stack(self):
        binary = self.build('process')
        # Inlining getrusage into different main-frame slots caused COW faults.
        # Exercise ASLR plus environment-size variants at stack page boundaries.
        for i in range(256):
            env = dict(os.environ, BOP_STACK_PADDING='x' * (i*64))
            run = subprocess.run([str(binary), '--cpu', '6', '--case', 'local_n' if i % 2 else 'untrained',
                                  '--rounds', '2'],
                                 env=env, text=True, capture_output=True, check=True, timeout=15)
            row, = csv.DictReader(io.StringIO(run.stdout), delimiter='\t')
            self.assertEqual(int(row['critical_faults_a']) + int(row['critical_faults_b']), 0,
                             f'fork stack variant {i}')


if __name__ == '__main__':
    unittest.main()
