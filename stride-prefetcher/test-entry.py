import argparse
import os
import subprocess
import sys


SRC = "test-entry.c"
OUT = "bin/test-entry"
DEFAULT_BASE_PC = "0x500000120"
DEFAULT_TARGET_BUFFER = "0x600000000"
DEFAULT_MAX_COMPETITORS = 128
DEFAULT_ROUNDS = 1000
DEFAULT_CORE = 2
DEFAULT_RESULT = "res/entry.tsv"


def compile_test():
    os.makedirs(os.path.dirname(OUT), exist_ok=True)
    compile_cmd = [
        "gcc",
        "-std=gnu11",
        "-O2",
        "-static",
        "-o",
        OUT,
        SRC,
    ]
    return subprocess.run(compile_cmd)


def run_test(args):
    run_cmd = [
        "taskset",
        "-c",
        str(args.core),
        "./" + OUT,
        args.base_pc,
        args.target_buffer,
        str(args.max_competitors),
        str(args.rounds),
    ]
    return subprocess.run(run_cmd, capture_output=True, text=True)


def save_result(path, output):
    if not path:
        return
    result_dir = os.path.dirname(path)
    if result_dir:
        os.makedirs(result_dir, exist_ok=True)
    with open(path, "w") as f:
        f.write(output)


def main():
    parser = argparse.ArgumentParser(description="Compile and run test-entry.c.")
    parser.add_argument("--base-pc", default=DEFAULT_BASE_PC)
    parser.add_argument("--target-buffer", default=DEFAULT_TARGET_BUFFER)
    parser.add_argument("--max-competitors", type=int, default=DEFAULT_MAX_COMPETITORS)
    parser.add_argument("--rounds", type=int, default=DEFAULT_ROUNDS)
    parser.add_argument("--core", type=int, default=DEFAULT_CORE)
    parser.add_argument("--output", default=DEFAULT_RESULT)
    parser.add_argument("--no-compile", action="store_true")
    args = parser.parse_args()

    if not args.no_compile:
        res = compile_test()
        if res.returncode != 0:
            print("Compile failed", file=sys.stderr)
            return res.returncode

    run = run_test(args)
    if run.stdout:
        print(run.stdout, end="")
    if run.stderr:
        print(run.stderr, end="", file=sys.stderr)
    if run.returncode != 0:
        print("Execution failed", file=sys.stderr)
        return run.returncode

    save_result(args.output, run.stdout)
    if args.output:
        print(f"Saved result to {args.output}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
