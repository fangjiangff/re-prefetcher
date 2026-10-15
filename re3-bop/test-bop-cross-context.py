#!/usr/bin/env python3
"""Run same-core thread/process BOP experiments as independent subprocesses."""
import argparse
import csv
import hashlib
import io
import json
from pathlib import Path
import random
import subprocess

HERE = Path(__file__).resolve().parent
RESULT_DIR = HERE / 'res'
ARCH_CONFIG = {
    'X925': {'cpu': 6, 'threshold_ns': 80, 'part': 0xd85,
             'l2_prefetch_event': 0x8155},
    'A725': {'cpu': 4, 'threshold_ns': 100, 'part': 0xd87,
             'l2_prefetch_event': 0x8285},
}
CASES = ('untrained', 'local_n', 'local_nm', 'switch_none', 'switch_local_nm',
         'split_same', 'split_other', 'b_only', 'no_trigger', 'retain_local',
         'retain_switch', 'retain_peer', 'untrained_peer', 'peer_local', 'split_peer')


def validate_faults(row):
    if int(row['critical_faults_a']) or int(row['critical_faults_b']):
        raise RuntimeError('critical page fault detected; raw row retained, no final summary; investigate before rerunning')


def aggregate(rows):
    fields = ('arch', 'mode', 'case', 'measure', 'cpu_a', 'n', 'm', 'full',
              'offset', 'probe_batch')
    groups = {}
    for row in rows:
        groups.setdefault(tuple(row[f] for f in fields), []).append(row)
    result = []
    for key, group in sorted(groups.items()):
        item = dict(zip(fields, key))
        probes = sum(int(r['probes']) for r in group)
        hits = sum(int(r['hits']) for r in group)
        item.update(repeats=len(group), probes=probes, hits=hits,
                    hit_pct=100*hits/probes if probes else 0,
                    hit_pct_min=min(float(r['hit_pct']) for r in group),
                    hit_pct_max=max(float(r['hit_pct']) for r in group))
        for f in ('avg_ns', 'a_event0_per_load', 'a_event1_per_load',
                  'b_event0_per_load', 'b_event1_per_load', 'handoff_us'):
            item[f + '_mean'] = sum(float(r[f]) for r in group) / len(group)
        for f in ('critical_faults_a', 'critical_faults_b'):
            item[f] = sum(int(r[f]) for r in group)
        result.append(item)
    return result


def parse_args(argv=None):
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--build', action='store_true')
    p.add_argument('--arch', choices=ARCH_CONFIG, default='X925')
    p.add_argument('--modes', default='thread,process')
    p.add_argument('--cpu', type=int)
    p.add_argument('--train-a-pairs', type=int, default=300)
    p.add_argument('--train-b-pairs', default='300')
    p.add_argument('--full-pairs', type=int, default=1500)
    p.add_argument('--offset-lines', type=int, default=16)
    p.add_argument('--other-offset', type=int, default=13)
    p.add_argument('--rounds', type=int, default=30)
    p.add_argument('--repeats', type=int, default=3)
    p.add_argument('--probe-batch', type=int, default=64)
    p.add_argument('--threshold-ns', type=int)
    p.add_argument('--seed', type=int, default=20260913)
    p.add_argument('--cases', default=','.join(CASES))
    p.add_argument('--measures', default='cache')
    p.add_argument('--output', type=Path)
    args = p.parse_args(argv)
    config = ARCH_CONFIG[args.arch]
    if args.cpu is None:
        args.cpu = config['cpu']
    if args.threshold_ns is None:
        args.threshold_ns = config['threshold_ns']
    modes, cases, measures = args.modes.split(','), args.cases.split(','), args.measures.split(',')
    try:
        amounts = [int(m) for m in args.train_b_pairs.split(',')]
    except ValueError:
        p.error('train-b-pairs must be comma-separated integers')
    if not set(modes) <= {'thread', 'process'} or not set(cases) <= set(CASES):
        p.error('invalid mode or case')
    if not set(measures) <= {'cache', 'demand', 'prefetch'}:
        p.error('invalid measure')
    if any(len(x) != len(set(x)) for x in (modes, cases, measures, amounts)):
        p.error('duplicate modes/cases/measures/amounts')
    if any(not 0 <= n <= 4096 for n in amounts + [args.train_a_pairs, args.full_pairs]):
        p.error('pair counts must be in 0..4096')
    if (set(cases) & {'local_nm', 'switch_local_nm'} and
            any(args.train_a_pairs + m > 4096 for m in amounts)):
        p.error('local N+M pair count must fit the single 4096-page training pool')
    if not 0 <= args.cpu < 1024 or not 1 <= args.rounds <= 10000 or args.repeats < 1:
        p.error('invalid CPU, rounds or repeats')
    if args.threshold_ns <= 0 or not 1 <= args.probe_batch <= 64:
        p.error('invalid threshold or probe-batch')
    if not (1 <= args.offset_lines < 64 and 1 <= args.other_offset < 64) or args.offset_lines == args.other_offset:
        p.error('offsets must be different values in 1..63')
    return args


def result_paths(output):
    return (output, output.with_name(output.stem + '-summary.tsv'),
            output.with_suffix('.json'))


def choose_output_path(args, result_dir=RESULT_DIR):
    if args.output is not None:
        conflicts = [path for path in result_paths(args.output) if path.exists()]
        if conflicts:
            raise FileExistsError(f'output exists: {conflicts[0]}')
        return args.output
    result_dir.mkdir(parents=True, exist_ok=True)
    original = result_dir / (
        f'bop-{args.arch}-cpu{args.cpu}-off{args.offset_lines}-cross-context.tsv')
    output = original
    run = 2
    while any(path.exists() for path in result_paths(output)):
        output = original.with_name(f'{original.stem}-run{run}{original.suffix}')
        run += 1
    return output


def validate_midr(arch, midr):
    expected = ARCH_CONFIG[arch]['part']
    actual = (midr >> 4) & 0xfff
    if actual != expected:
        raise ValueError(
            f'selected {arch} requires MIDR part {expected:#x}, found {actual:#x}')


def validate_measure_environment(measures, module_loaded):
    if module_loaded and set(measures) != {'cache'}:
        raise ValueError('unload prefetch_pmu_event before perf; restore it afterwards')


def build_command(binary, source, arch):
    event = ARCH_CONFIG[arch]['l2_prefetch_event']
    return ['gcc', '-std=gnu11', '-O2', '-Wall', '-Wextra', '-Werror',
            '-march=armv8.5-a+predres', '-pthread',
            f'-DCC_L2_PREFETCH_EVENT={event:#x}', str(source), '-o', str(binary)]


def main():
    args = parse_args()
    modes, cases, measures = args.modes.split(','), args.cases.split(','), args.measures.split(',')
    amounts = [int(m) for m in args.train_b_pairs.split(',')]
    try:
        args.output = choose_output_path(args)
    except FileExistsError as error:
        raise SystemExit(str(error)) from error
    summary_path, metadata_path = result_paths(args.output)[1:]
    module_loaded = Path('/sys/module/prefetch_pmu_event').exists()
    try:
        validate_measure_environment(measures, module_loaded)
    except ValueError as error:
        raise SystemExit(str(error)) from error
    root = Path(f'/sys/devices/system/cpu/cpu{args.cpu}')
    midr = int((root / 'regs/identification/midr_el1').read_text(), 16)
    try:
        validate_midr(args.arch, midr)
    except ValueError as error:
        raise SystemExit(str(error)) from error
    cache = [{k: (index/k).read_text().strip() for k in ('level', 'type', 'size', 'shared_cpu_list')}
             for index in sorted((root/'cache').glob('index*'))]
    binaries = {mode: HERE/f'test-bop-cross-{mode}' for mode in modes}
    if args.build:
        for binary in binaries.values():
            subprocess.run(build_command(binary, binary.with_suffix('.c'), args.arch), check=True)
    files = [Path(__file__), HERE/'bop-cross-context.h', HERE.parent/'until.h']
    files += list(binaries.values()) + [b.with_suffix('.c') for b in binaries.values()]
    metadata = {k: str(v) if isinstance(v, Path) else v for k, v in vars(args).items()}
    metadata.update(midr=hex(midr), cache=cache, pmu_module_loaded=module_loaded,
                    sha256={str(f): hashlib.sha256(f.read_bytes()).hexdigest() for f in files})
    args.output.parent.mkdir(parents=True, exist_ok=True)
    with metadata_path.open('x') as stream:
        json.dump(metadata, stream, indent=2); stream.write('\n')
    jobs = [(mode, m, case) for mode in modes for m in amounts for case in cases]
    rows = []
    with args.output.open('x', newline='') as stream:
        writer = None
        for measure in measures:
            for repeat in range(args.repeats):
                order = jobs.copy(); random.Random(args.seed + repeat).shuffle(order)
                for index, (mode, m, case) in enumerate(order):
                    cmd = [str(binaries[mode]), '--cpu', str(args.cpu), '--case', case,
                           '--train-a-pairs', str(args.train_a_pairs), '--train-b-pairs', str(m),
                           '--full-pairs', str(args.full_pairs), '--offset-lines', str(args.offset_lines),
                           '--other-offset', str(args.other_offset), '--rounds', str(args.rounds),
                           '--probe-batch', str(args.probe_batch),
                           '--threshold-ns', str(args.threshold_ns),
                           '--seed', str(args.seed + repeat), '--measure', measure]
                    run = subprocess.run(cmd, text=True, capture_output=True, timeout=125)
                    if run.returncode:
                        raise RuntimeError(f'{cmd}\nexit={run.returncode}\n{run.stderr}')
                    data = list(csv.DictReader(io.StringIO(run.stdout), delimiter='\t'))
                    if len(data) != 1:
                        raise RuntimeError(f'invalid result: {run.stdout}')
                    row = dict(repeat=repeat+1, arch=args.arch, **data[0])
                    if row['cpu_a'] != str(args.cpu) or row['cpu_b'] != str(args.cpu):
                        raise RuntimeError('CPU mismatch')
                    if (row['pid_a'] == row['pid_b']) != (mode == 'thread') or row['tid_a'] == row['tid_b']:
                        raise RuntimeError('context identity mismatch')
                    if writer is None:
                        writer = csv.DictWriter(stream, fieldnames=list(row), delimiter='\t'); writer.writeheader()
                    writer.writerow(row); stream.flush(); rows.append(row)
                    validate_faults(row)
                    if (index+1) % 15 == 0 or index+1 == len(order):
                        print(f'{measure}: repeat {repeat+1}/{args.repeats}, {index+1}/{len(order)}', flush=True)
    summary = aggregate(rows)
    with summary_path.open('x', newline='') as stream:
        writer = csv.DictWriter(stream, fieldnames=list(summary[0]), delimiter='\t')
        writer.writeheader(); writer.writerows(summary)
    print(summary_path)


if __name__ == '__main__':
    main()
