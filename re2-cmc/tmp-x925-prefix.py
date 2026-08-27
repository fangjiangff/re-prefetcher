#!/usr/bin/env python3
import argparse
import pathlib
import subprocess


HERE = pathlib.Path(__file__).resolve().parent
PMU_MODULE_DIR = HERE.parent / "kernel-modules" / "prefetch-pmu-event"
PMU_MODULE_NAME = "prefetch_pmu_event"


def module_script(name, *arguments):
    subprocess.run([
        "sudo", str(PMU_MODULE_DIR / name),
        *(str(argument) for argument in arguments),
    ], check=True)


def load_pmu(cpu):
    module_path = pathlib.Path("/sys/module") / PMU_MODULE_NAME
    if module_path.is_dir():
        raise RuntimeError(f"{PMU_MODULE_NAME} is already loaded")
    module_script("module_load.sh", "--cpu", cpu)


def unload_pmu():
    module_script("module_unload.sh")


def main():
    parser = argparse.ArgumentParser(description="Temporary X925 CMC prefix scan")
    parser.add_argument("--cpu", type=int, default=6)
    parser.add_argument("--samples", type=int, default=100)
    parser.add_argument("--training-replays", type=int, default=8)
    parser.add_argument("--threshold", type=int, default=80)
    parser.add_argument("--start", type=int, default=0)
    parser.add_argument("--nodes", type=int, choices=(64, 128, 192), default=64)
    parser.add_argument("--mode", choices=("exact", "reverse", "unrelated", "all", "grid", "targets", "pcs", "activation"),
                        default="exact")
    parser.add_argument("--pmu", action=argparse.BooleanOptionalAction,
                        default=False)
    args = parser.parse_args()

    binary = pathlib.Path("/tmp/test-x925-cmc-prefix")
    subprocess.run([
        "gcc", "-O2", "-Wall", "-Wextra", "-march=armv8.5-a+predres",
        f"-DNODE_COUNT={args.nodes}",
        f"-DENABLE_PMU={int(args.pmu)}",
        str(HERE / "tmp-x925-prefix.c"), "-o", str(binary),
    ], check=True)
    command = [
        "taskset", "-c", str(args.cpu), str(binary),
        str(args.samples), str(args.training_replays), str(args.threshold),
        str(args.start),
        str({"exact": 0, "reverse": 1, "unrelated": 2, "all": 3,
             "grid": 4, "targets": 5, "pcs": 6,
             "activation": 7}[args.mode]),
    ]
    loaded = False
    try:
        if args.pmu:
            load_pmu(args.cpu)
            loaded = True
        subprocess.run(command, check=True)
    finally:
        if loaded:
            unload_pmu()


if __name__ == "__main__":
    main()
