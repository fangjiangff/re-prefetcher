import argparse
import os
import subprocess
import sys


TEST_THRESHOLD = "testThreshold2.py"
SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
DEFAULT_ARCH = "A76"
DEFAULT_CORE = 2
# DEFAULT_STRIDES = [8,14,30]
# DEFAULT_STRIDES = [1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31,32]
# DEFAULT_STRIDES = [30, 1, 5, 14]
DEFAULT_STRIDES = [7,9,11,13]
DEFAULT_OUTPUT_DIR = "res/probeNext64Lines"

def arch_dir_name(arch):
    return arch.replace(os.sep, "_")


def parse_stride_list(value):
    cleaned = value.strip()
    if cleaned.startswith("[") and cleaned.endswith("]"):
        cleaned = cleaned[1:-1]

    strides = []
    for item in cleaned.split(","):
        item = item.strip()
        if not item:
            continue
        stride = int(item)
        if stride < 1:
            raise argparse.ArgumentTypeError("stride values must be >= 1")
        strides.append(stride)

    if not strides:
        raise argparse.ArgumentTypeError("at least one stride is required")
    return strides


def parse_args():
    parser = argparse.ArgumentParser(
        description="Run testThreshold2.py for a list of stride values."
    )
    parser.add_argument(
        "--stride",
        "--strides",
        dest="strides",
        type=parse_stride_list,
        default=DEFAULT_STRIDES,
        help="Comma-separated stride list, e.g. '[1,2,5,10,15,30]'.",
    )
    parser.add_argument("--arch", default=DEFAULT_ARCH)
    parser.add_argument("--core", type=int, default=DEFAULT_CORE)
    parser.add_argument("--output-dir", default=DEFAULT_OUTPUT_DIR)
    parser.add_argument("--plot-only", action="store_true")
    return parser.parse_args()


def run_one_stride(args, stride):
    result_dir = os.path.join(args.output_dir, arch_dir_name(args.arch))
    heatmap_dir = os.path.join(result_dir, "heatmaps")
    cmd = [
        sys.executable,
        os.path.join(SCRIPT_DIR, TEST_THRESHOLD),
        "--stride",
        str(stride),
        "--arch",
        args.arch,
        "--core",
        str(args.core),
        "--output-dir",
        result_dir,
        "--heatmap-dir",
        heatmap_dir,
    ]
    if args.plot_only:
        cmd.append("--plot-only")

    print("=" * 60)
    print(f"Running stride={stride}: {' '.join(cmd)}")
    return subprocess.run(cmd, cwd=SCRIPT_DIR)


def main():
    args = parse_args()
    result_dir = os.path.join(args.output_dir, arch_dir_name(args.arch))
    os.makedirs(result_dir, exist_ok=True)

    for stride in args.strides:
        res = run_one_stride(args, stride)
        if res.returncode != 0:
            print(f"Failed at stride={stride}", file=sys.stderr)
            return res.returncode

    print(f"All done. Results and heatmaps are under {result_dir}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
