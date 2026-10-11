#!/usr/bin/env python3
"""Run the EL0/EL1 BOP cases as independent processes."""

import argparse
import csv
import subprocess
from pathlib import Path


CASE_NAMES = (
    "untrained_kernel",
    "el0_only_user_trigger",
    "kernel_trigger",
    "kernel_train_user_trigger",
    "kernel_stride_user_trigger",
)
HERE = Path(__file__).resolve().parent
ARCH_CONFIG = {
    "X925": {"cpu": 6, "threshold_ns": 80, "part": 0xD85},
    "A725": {"cpu": 4, "threshold_ns": 100, "part": 0xD87},
}


def make_parser():
    parser = argparse.ArgumentParser(
        description=(
            "Run the EL0/EL1 BOP cases in separate processes and compare "
            "X+D cache hits."
        )
    )
    parser.add_argument("--arch", choices=ARCH_CONFIG, default="X925")
    parser.add_argument("--binary", default=str(HERE / "test_bop_el0_el1"))
    parser.add_argument("--device", default="/dev/bop_el0_el1")
    parser.add_argument("--cpu", type=int)
    parser.add_argument("--rounds", type=int, default=30)
    parser.add_argument("--train-pairs", type=int, default=300)
    parser.add_argument("--kernel-train-pairs", type=int, default=300)
    parser.add_argument("--offset-lines", type=int, default=16)
    parser.add_argument("--threshold-ns", type=int)
    parser.add_argument("--output", help="write summary TSV to this path")
    parser.add_argument("--build", action="store_true", help="run make probe first")
    parser.add_argument("--no-sudo", action="store_true",
                        help="do not prefix commands with sudo")
    return parser


def parse_args(argv=None):
    args = make_parser().parse_args(argv)
    config = ARCH_CONFIG[args.arch]
    if args.cpu is None:
        args.cpu = config["cpu"]
    if args.threshold_ns is None:
        args.threshold_ns = config["threshold_ns"]
    return args


def validate_midr(arch, midr):
    expected = ARCH_CONFIG[arch]["part"]
    actual = (midr >> 4) & 0xFFF
    if actual != expected:
        raise ValueError(
            f"selected {arch} requires MIDR part {expected:#x}, found {actual:#x}"
        )


def read_cpu_midr(cpu):
    path = Path(f"/sys/devices/system/cpu/cpu{cpu}/regs/identification/midr_el1")
    return int(path.read_text().strip(), 16)


def case_commands(binary, cpu, device, rounds, train_pairs, offset_lines,
                  threshold_ns, use_sudo=True, kernel_train_pairs=0):
    prefix = [] if not use_sudo else ["sudo"]
    commands = []
    for case_name in CASE_NAMES:
        command = prefix + [
            "taskset", "-c", str(cpu), binary,
            "--case", case_name,
            "--device", device,
            "--cpu", str(cpu),
            "--rounds", str(rounds),
            "--train-pairs", str(train_pairs),
            "--offset-lines", str(offset_lines),
            "--threshold-ns", str(threshold_ns),
        ]
        if kernel_train_pairs:
            command.extend(["--kernel-train-pairs", str(kernel_train_pairs)])
        commands.append(command)
    return commands


def parse_result(stdout):
    for line in stdout.splitlines():
        fields = line.split("\t")
        if len(fields) == 4 and fields[0] in CASE_NAMES:
            return fields
    raise RuntimeError("case result row not found in program output")


def write_summary(output, arch, results):
    with Path(output).open("w", newline="") as stream:
        writer = csv.writer(stream, delimiter="\t", lineterminator="\n")
        writer.writerow(["arch", "case", "probes", "hits", "hits_per_1000"])
        writer.writerows([[arch, *result] for result in results])


def main(argv=None):
    args = parse_args(argv)

    try:
        validate_midr(args.arch, read_cpu_midr(args.cpu))
    except (OSError, ValueError) as error:
        raise SystemExit(str(error)) from error

    if args.build:
        subprocess.run(["make", "probe"], cwd=HERE, check=True)

    results = []
    for command in case_commands(
        args.binary, args.cpu, args.device, args.rounds, args.train_pairs,
        args.offset_lines, args.threshold_ns, not args.no_sudo,
        args.kernel_train_pairs,
    ):
        completed = subprocess.run(command, text=True, capture_output=True)
        if completed.stdout:
            print(completed.stdout, end="")
        if completed.returncode != 0:
            if completed.stderr:
                print(completed.stderr, end="")
            raise SystemExit(completed.returncode)
        results.append(parse_result(completed.stdout))

    print("summary_arch\tcase\tprobes\thits\thits_per_1000")
    for result in results:
        print("\t".join([args.arch, *result]))
    if args.output:
        write_summary(args.output, args.arch, results)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
