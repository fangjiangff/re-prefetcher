import argparse
import csv
import subprocess
import sys
from pathlib import Path

from cross_test_config import ARCH_CONFIG, apply_threshold_defaults, arch_choices


BASE_DIR = Path(__file__).resolve().parent
GREEN = "\033[32m"
RED = "\033[31m"
RESET = "\033[0m"

TESTS = [
    {
        "name": "cross-process",
        "script": "test-cross-process.py",
        "result_dir": BASE_DIR / "res" / "cross-process-strong",
        "needs_cores": False,
    },
    {
        "name": "cross-thread",
        "script": "test-cross-thread.py",
        "result_dir": BASE_DIR / "res" / "cross-thread",
        "needs_cores": False,
    },
    {
        "name": "cross-el0-el1",
        "script": "test-cross-el0-el1.py",
        "result_dir": BASE_DIR / "res" / "cross-el0-el1",
        "needs_cores": False,
    },
    {
        "name": "cross-trustzone",
        "script": "test-cross-trustzone.py",
        "result_dir": BASE_DIR / "res" / "cross-trustzone",
        "needs_cores": False,
    },
    {
        "name": "cross-core",
        "script": "test-cross-core.py",
        "result_dir": BASE_DIR / "res" / "cross-core",
        "needs_cores": True,
    },
]

NON_TRIGGER_MARKERS = (
    "-no-trigger-control",
    "-same-process-baseline",
    "-process-switch-baseline",
    "-context-switch-baseline",
    "-same-el0-baseline",
    "-no-secure-switch-baseline",
    "-secure-noop-ns-trigger",
)


def parse_args():
    parser = argparse.ArgumentParser(
        description="Run all cross-context tests and report predicted-line retention."
    )
    parser.add_argument("--arch", required=True, choices=arch_choices())
    parser.add_argument("--core", type=int,
                        help="Override core for non-cross-core tests.")
    parser.add_argument("--train-core", type=int,
                        help="Override training core for cross-core test.")
    parser.add_argument("--trigger-core", type=int,
                        help="Override trigger core for cross-core test.")
    parser.add_argument("--access", choices=["store", "load"], default="store")
    parser.add_argument("--threshold-ns", type=int, default=None,
                        help="predicted avg_ns threshold. Default is selected from --arch.")
    parser.add_argument("--stride", type=int)
    parser.add_argument("--train-accesses", type=int)
    parser.add_argument("--rounds", type=int)
    parser.add_argument("--probe-positions", type=int)
    parser.add_argument("--hit-threshold-ns", type=int)
    parser.add_argument("--cc")
    parser.add_argument("--plot", action="store_true",
                        help="Allow each test wrapper to generate plots.")
    parser.add_argument("--skip-trustzone", action="store_true",
                        help="Skip TrustZone test if OP-TEE is unavailable.")
    args = parser.parse_args()

    apply_threshold_defaults(args)

    if args.core is not None and args.core < 0:
        parser.error("--core must be >= 0")
    if args.train_core is not None and args.train_core < 0:
        parser.error("--train-core must be >= 0")
    if args.trigger_core is not None and args.trigger_core < 0:
        parser.error("--trigger-core must be >= 0")
    if (args.train_core is not None and args.trigger_core is not None and
            args.train_core == args.trigger_core):
        parser.error("--train-core and --trigger-core must be different")
    if args.threshold_ns < 1:
        parser.error("--threshold-ns must be >= 1")
    return args


def trigger_tsvs(result_dir):
    if not result_dir.exists():
        return {}
    return {
        path: path.stat().st_mtime_ns
        for path in result_dir.glob("*.tsv")
        if not any(marker in path.name for marker in NON_TRIGGER_MARKERS)
    }


def newest_trigger_tsv(test, before):
    after = trigger_tsvs(test["result_dir"])
    candidates = [
        path
        for path, mtime in after.items()
        if path not in before or before[path] != mtime
    ]
    if not candidates:
        return None
    return max(candidates, key=lambda path: after[path])


def predicted_avg_ns(tsv_path):
    with tsv_path.open(newline="") as f:
        reader = csv.DictReader(f, delimiter="\t")
        for row in reader:
            if row.get("role") == "predicted":
                return int(row["avg_ns"])
    raise ValueError(f"missing predicted row in {tsv_path}")


def add_optional(cmd, flag, value):
    if value is not None:
        cmd.extend([flag, str(value)])


def colored_yes_no(value):
    text = "yes" if value else "no"
    color = GREEN if value else RED
    return f"{color}{text}{RESET}"


def context_for_test(test, args):
    config = ARCH_CONFIG[args.arch]
    if test["needs_cores"]:
        cross_core = config["cross_core"]
        train_core = (
            args.train_core
            if args.train_core is not None
            else cross_core["train_core"]
        )
        trigger_core = (
            args.trigger_core
            if args.trigger_core is not None
            else cross_core["trigger_core"]
        )
        return (
            f"arch={args.arch}, train_core={train_core}, "
            f"trigger_core={trigger_core}"
        )

    core = args.core if args.core is not None else config["core"]
    return f"arch={args.arch}, core={core}"


def command_for_test(test, args):
    cmd = [
        sys.executable,
        str(BASE_DIR / test["script"]),
        "--arch",
        args.arch,
        "--access",
        args.access,
    ]

    if not args.plot:
        cmd.append("--no-plot")

    if test["needs_cores"]:
        add_optional(cmd, "--train-core", args.train_core)
        add_optional(cmd, "--trigger-core", args.trigger_core)
    else:
        add_optional(cmd, "--core", args.core)

    add_optional(cmd, "--stride", args.stride)
    add_optional(cmd, "--train-accesses", args.train_accesses)
    add_optional(cmd, "--rounds", args.rounds)
    add_optional(cmd, "--probe-positions", args.probe_positions)
    add_optional(cmd, "--hit-threshold-ns", args.hit_threshold_ns)
    add_optional(cmd, "--cc", args.cc)

    if test["name"] == "cross-trustzone":
        cmd.insert(0, "sudo")

    return cmd


def run_test(test, args):
    before = trigger_tsvs(test["result_dir"])
    cmd = command_for_test(test, args)
    print("=" * 60)
    print(f"Running {test['name']}: {' '.join(cmd)}")

    run = subprocess.run(cmd, cwd=BASE_DIR, text=True,
                         capture_output=True)
    if run.stdout:
        print(run.stdout, end="")
    if run.stderr:
        print(run.stderr, end="", file=sys.stderr)
    if run.returncode != 0:
        return {
            "name": test["name"],
            "ok": False,
            "error": f"exit code {run.returncode}",
        }

    tsv_path = newest_trigger_tsv(test, before)
    if tsv_path is None:
        return {
            "name": test["name"],
            "ok": False,
            "error": "no trigger TSV generated",
        }

    try:
        avg_ns = predicted_avg_ns(tsv_path)
    except (OSError, ValueError) as exc:
        return {
            "name": test["name"],
            "ok": False,
            "error": str(exc),
        }

    supported = avg_ns < args.threshold_ns
    return {
        "name": test["name"],
        "ok": True,
        "avg_ns": avg_ns,
        "supported": supported,
        "tsv": tsv_path,
        "context": context_for_test(test, args),
    }


def main():
    args = parse_args()
    tests = [
        test for test in TESTS
        if not (args.skip_trustzone and test["name"] == "cross-trustzone")
    ]
    results = [run_test(test, args) for test in tests]

    print("=" * 60)
    print(f"predicted threshold: < {args.threshold_ns} ns")
    had_error = False
    for result in results:
        if not result["ok"]:
            had_error = True
            print(f"{result['name']}: error ({result['error']})")
            continue
        print(
            f"{result['name']}: {colored_yes_no(result['supported'])} "
            f"({result['context']})"
            # f"(predicted_avg_ns={result['avg_ns']}, tsv={result['tsv']})"
        )

    return 1 if had_error else 0


if __name__ == "__main__":
    sys.exit(main())
