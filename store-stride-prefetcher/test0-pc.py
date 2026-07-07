import argparse
import os
import subprocess
import sys

from cross_test_config import ARCH_CONFIG, arch_choices


BASE_DIR = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.join(BASE_DIR, "test0-pc.c")
UTIL_SRC = os.path.join(BASE_DIR, "until.c")
OUT = os.path.join(BASE_DIR, "bin", "test0-pc")

RESULT_DIR = os.path.join(BASE_DIR, "res", "store-stride")
RAW_DIR = os.path.join(RESULT_DIR, "raw")
DUMP_DIR = os.path.join(RESULT_DIR, "dump")


def parse_args():
    parser = argparse.ArgumentParser(
        description="Run test0-pc with four controlled training store PCs."
    )
    parser.add_argument("--arch", choices=arch_choices(), default="A76")
    parser.add_argument("--core", type=int, default=None)
    parser.add_argument("--stride", type=int, default=5,
                        help="Stride in cache lines. Default: 5")
    parser.add_argument("--train-step", type=int, default=3)
    parser.add_argument("--rounds", type=int, default=40000)
    parser.add_argument("--probe-positions", type=int, default=100)
    parser.add_argument("--dummy-buffer-pages", type=int, default=10)
    parser.add_argument("--context-switch", action="store_true")
    parser.add_argument("--context-switch-yields", type=int, default=1)
    parser.add_argument("--train-pc0", default="0x783709b0120")
    parser.add_argument("--train-pc1", default="0x2d650271c2a4")
    parser.add_argument("--train-pc2", default="0x646f3e8ac548")
    parser.add_argument("--train-pc3", default="0x3a74fdac8a90")
    parser.add_argument("--cc", default=os.environ.get("CC", "gcc"))
    parser.add_argument("--objdump", default=os.environ.get("OBJDUMP", "objdump"))
    parser.add_argument("--no-compile", action="store_true")
    parser.add_argument("--no-dump", action="store_true")
    args = parser.parse_args()

    if args.core is None:
        args.core = ARCH_CONFIG[args.arch]["core"]
    if args.core < 0:
        parser.error("--core must be >= 0")
    if args.stride < 1:
        parser.error("--stride must be >= 1")
    if args.train_step < 1:
        parser.error("--train-step must be >= 1")
    if args.rounds < 0:
        parser.error("--rounds must be >= 0")
    if args.probe_positions < 1:
        parser.error("--probe-positions must be >= 1")
    if args.dummy_buffer_pages < 1:
        parser.error("--dummy-buffer-pages must be >= 1")
    if args.context_switch_yields < 1:
        parser.error("--context-switch-yields must be >= 1")
    if args.train_step * args.stride >= args.probe_positions:
        parser.error("predicted position train_step * stride must be inside probe positions")

    for name in ("train_pc0", "train_pc1", "train_pc2", "train_pc3"):
        value = int(getattr(args, name), 0)
        if value < 4 or value % 4 != 0:
            parser.error(f"--{name.replace('_', '-')} must be 4-byte aligned and >= 4")
        setattr(args, name, value)

    return args


def ensure_dirs():
    os.makedirs(os.path.dirname(OUT), exist_ok=True)
    os.makedirs(RAW_DIR, exist_ok=True)
    os.makedirs(DUMP_DIR, exist_ok=True)


def arch_cflags_for(arch):
    if arch in {"x86", "Zen4"}:
        return []
    return ["-march=armv8.5-a+predres"]


def run_name(args):
    switch_suffix = (
        f"-ctxswitch{args.context_switch_yields}"
        if args.context_switch else ""
    )
    return (
        f"{args.arch}-core{args.core}-stride{args.stride}"
        f"-train{args.train_step}-store-pc{switch_suffix}"
    )


def compile_test(args):
    cmd = [
        args.cc,
        "-std=gnu11",
        "-O0",
        "-static",
        f"-DSTRIDE_BYTES={args.stride * 64}",
        f"-DTRAIN_STEP={args.train_step}",
        f"-DROUNDS={args.rounds}",
        f"-DPROBE_POSITIONS={args.probe_positions}",
        f"-DDUMMY_BUFFER_PAGES={args.dummy_buffer_pages}",
        f"-DCONTEXT_SWITCH_BEFORE_TRIGGER={1 if args.context_switch else 0}",
        f"-DCONTEXT_SWITCH_YIELDS={args.context_switch_yields}",
        f"-DTRAIN_STORE_PC0={args.train_pc0:#x}ull",
        f"-DTRAIN_STORE_PC1={args.train_pc1:#x}ull",
        f"-DTRAIN_STORE_PC2={args.train_pc2:#x}ull",
        f"-DTRAIN_STORE_PC3={args.train_pc3:#x}ull",
        "-o",
        OUT,
        SRC,
        UTIL_SRC,
    ]
    cmd[1:1] = arch_cflags_for(args.arch)
    print(" ".join(cmd))
    return subprocess.run(cmd).returncode


def write_dump(args):
    if args.no_dump:
        return

    path = os.path.join(DUMP_DIR, run_name(args) + ".dump")
    run = subprocess.run([args.objdump, "-d", OUT],
                         capture_output=True, text=True)
    if run.returncode != 0:
        print("failed to generate dump", file=sys.stderr)
        if run.stderr:
            print(run.stderr, file=sys.stderr)
        return

    with open(path, "w") as f:
        f.write(run.stdout)
    print(f"Saved dump to {path}")


def run_binary(args):
    return subprocess.run(["taskset", "-c", str(args.core), OUT],
                          capture_output=True, text=True)


def main():
    args = parse_args()
    ensure_dirs()

    if not args.no_compile and compile_test(args) != 0:
        return 1

    write_dump(args)
    run = run_binary(args)

    if run.stdout:
        print(run.stdout, end="")
    if run.stderr:
        print(run.stderr, file=sys.stderr, end="")

    if run.returncode != 0:
        print(f"Execution failed with return code {run.returncode}",
              file=sys.stderr)
        return run.returncode

    raw_path = os.path.join(RAW_DIR, run_name(args) + ".txt")
    with open(raw_path, "w") as f:
        f.write(run.stdout)
    print(f"Saved raw output to {raw_path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
