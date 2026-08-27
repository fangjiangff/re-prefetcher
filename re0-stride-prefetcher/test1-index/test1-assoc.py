import argparse
import csv
import os
import subprocess
import sys

ROOT_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
if ROOT_DIR not in sys.path:
    sys.path.insert(0, ROOT_DIR)

from cross_test_config import ARCH_CONFIG, arch_choices, is_x86_arch

BASE_DIR = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.join(BASE_DIR, "test1-assoc.c")
UTIL_SRC = os.path.join(ROOT_DIR, "until.c")
OUT = os.path.join(ROOT_DIR, "bin", "test1-assoc")
RESULT_DIR = os.path.join(ROOT_DIR, "res", "store-stride-assoc")
DEFAULT_MAX_COMPETITORS = 32
DEFAULT_ROUNDS = 4000
DEFAULT_CANDIDATE_PAGES = 8192
DEFAULT_TIMER = "cntvct"


def parse_args():
    parser = argparse.ArgumentParser(
        description="Run PA[12:15]-indexed store-stride associativity test."
    )
    parser.add_argument("--arch", choices=arch_choices(), required=True,
                        help="Architecture label used to select CPU core/compiler flags.")
    parser.add_argument("--core", type=int, default=None,
                        help="CPU core for taskset. Default comes from --arch.")
    parser.add_argument("--max-competitors", type=int, default=DEFAULT_MAX_COMPETITORS)
    parser.add_argument("--rounds", type=int, default=DEFAULT_ROUNDS)
    parser.add_argument("--candidate-pages", type=int, default=DEFAULT_CANDIDATE_PAGES)
    parser.add_argument("--timer", choices=["gettime", "rdtsc", "cntvct", "pmccntr"],
                        default=DEFAULT_TIMER,
                        help="Timestamp source. Default: cntvct")
    parser.add_argument("--cc", default=os.environ.get("CC", "gcc"))
    parser.add_argument("--no-compile", action="store_true")
    parser.add_argument("--no-taskset", action="store_true",
                        help="Run without CPU affinity (useful on systems without taskset).")
    parser.add_argument("--output", default=None,
                        help="Raw output path. Default: res/store-stride-assoc/<arch>.txt")
    args = parser.parse_args()
    if args.core is None:
        args.core = ARCH_CONFIG[args.arch]["core"]
    if args.core < 0 or args.max_competitors < 1 or args.rounds < 1:
        parser.error("core, max-competitors and rounds must be positive")
    if args.candidate_pages < args.max_competitors * 16:
        parser.error("candidate-pages is too small for the requested set candidates")
    supported_timers = {"gettime", "rdtsc"} if is_x86_arch(args.arch) else {"gettime", "cntvct", "pmccntr"}
    if args.timer not in supported_timers:
        parser.error(f"--timer {args.timer} is not supported for {args.arch}")
    return args


def timer_define(timer):
    return {
        "gettime": "-DGETTIME=1",
        "rdtsc": "-DRDTSC=1",
        "cntvct": "-DCNTVCT=1",
        "pmccntr": "-DPMCCNTR=1",
    }[timer]


def compile_test(args):
    os.makedirs(os.path.dirname(OUT), exist_ok=True)
    cmd = [args.cc, "-std=gnu11", "-O0", "-static", "-Wall", "-Wextra",
           f"-DMAX_COMPETITORS={args.max_competitors}",
           f"-DCANDIDATE_PAGES={args.candidate_pages}",
           timer_define(args.timer),
           "-o", OUT, SRC, UTIL_SRC]
    if args.arch not in {"x86", "Zen4"}:
        cmd.insert(5, "-march=armv8.5-a+predres")
    print("Compiling:", " ".join(cmd))
    return subprocess.run(cmd).returncode


def run_test(args):
    cmd = [OUT, str(args.max_competitors), str(args.rounds)]
    if not args.no_taskset:
        cmd = ["taskset", "-c", str(args.core)] + cmd
    print("Running:", " ".join(cmd))
    return subprocess.run(cmd, capture_output=True, text=True)


def save_results(args, output):
    os.makedirs(RESULT_DIR, exist_ok=True)
    raw_path = args.output or os.path.join(RESULT_DIR, f"{args.arch}-assoc.txt")
    with open(raw_path, "w") as handle:
        handle.write(output)

    rows = []
    for line in output.splitlines():
        fields = line.split()
        if len(fields) != 4 or fields[0] not in {"no_trigger", "same_set", "different_set"}:
            continue
        rows.append({"mode": fields[0], "competitors": int(fields[1]),
                     "avg": int(fields[2]), "probes": int(fields[3])})
    tsv_path = os.path.splitext(raw_path)[0] + ".tsv"
    if rows:
        with open(tsv_path, "w", newline="") as handle:
            writer = csv.DictWriter(handle, fieldnames=rows[0].keys(), delimiter="\t")
            writer.writeheader()
            writer.writerows(rows)
    print(f"Saved raw output to {raw_path}")
    if rows:
        print(f"Saved parsed results to {tsv_path}")
        for row in rows:
            print(f"{row['mode']:14s} competitors={row['competitors']:3d} "
                  f"avg={row['avg']} probes={row['probes']}")


def main():
    args = parse_args()
    if not args.no_compile and compile_test(args) != 0:
        return 1
    run = run_test(args)
    if run.returncode != 0:
        if run.stdout:
            print(run.stdout)
        if run.stderr:
            print(run.stderr, file=sys.stderr)
        return run.returncode
    save_results(args, run.stdout)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
