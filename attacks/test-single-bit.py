import subprocess
import argparse
import re
import time
import csv

import os

os.environ.setdefault("MPLCONFIGDIR", "/tmp/matplotlib")

SRC = "single-bit-cc.cc"
OUT = "bin/single-bit-cc"

DEFAULT_ARCH = "A76"
DEFAULT_CORE = 2
DEFAULT_BENCHMARK_ROUNDS = [100, 200, 400, 600, 800, 1000]
# timestamp = time.strftime("%Y-%m-%d-%H-%M-%S", time.localtime())
# print(f"Start testing at {timestamp}")


def timing_file(arch, core):
    return f"res/single-timing-{arch}-core{core}.txt"


def compile_binary(sw):
    compile_cmd = [
        "g++",
        "-std=gnu++17",
        "-O0",
        "-static",
        f"-DTEST_ON_SW={sw}",
        "-o",
        OUT,
        SRC
    ]
    return subprocess.run(compile_cmd)


def run_binary(core, rounds):
    start = time.perf_counter()
    run = subprocess.run(
        ["taskset", "-c", str(core), "./" + OUT, str(rounds)],
        capture_output=True,
        text=True
    )
    elapsed_s = time.perf_counter() - start
    return run, elapsed_s


def row_raw_output(row):
    raw_output = row.get("raw_output", "")
    if raw_output:
        return raw_output
    if row.get("correct") and row.get("total") and row.get("attack_elapsed_ns") and row.get("attack_elapsed_s"):
        output = (
            f"Accuracy: {row.get('correct')}/{row.get('total')} "
            f"Elapsed_ns: {row.get('attack_elapsed_ns')} "
            f"Elapsed_s: {row.get('attack_elapsed_s')}"
        )
        if row.get("data_rate_bytes_s"):
            output += f" Data_rate_Bytes_s: {row.get('data_rate_bytes_s')}"
        return output
    return ""


def parse_attack_metrics(output):
    accuracy_match = re.search(r"Accuracy:\s*(\d+)/(\d+)", output)
    elapsed_match = re.search(
        r"Elapsed_ns:\s*(\d+)\s*Elapsed_s:\s*([0-9.]+)(?:\s*(Bandwidth_bps|Data_rate_Bytes_s):\s*([0-9.]+))?",
        output
    )

    correct = int(accuracy_match.group(1)) if accuracy_match else None
    total = int(accuracy_match.group(2)) if accuracy_match else None
    accuracy = correct / total if correct is not None and total else None

    if not elapsed_match:
        return correct, total, accuracy, None, None, None

    data_rate_bytes_s = None
    if elapsed_match.group(4):
        value = float(elapsed_match.group(4))
        data_rate_bytes_s = value / 8.0 if elapsed_match.group(3) == "Bandwidth_bps" else value
    return (
        correct,
        total,
        accuracy,
        int(elapsed_match.group(1)),
        float(elapsed_match.group(2)),
        data_rate_bytes_s,
    )


def benchmark_time(arch, core, rounds_list):
    os.makedirs("res", exist_ok=True)
    output_file = timing_file(arch, core)
    rows = []

    for sw, mode_name in [(0, "load"), (1, "sw_prefetch")]:
        print("="*60)
        print(f"Training mode: {mode_name} (TEST_ON_SW={sw})")

        res = compile_binary(sw)
        if res.returncode != 0:
            print(f"Compile failed for mode {mode_name}")
            continue

        for rounds in rounds_list:
            print(f"Running rounds={rounds} on core={core}")
            run, process_elapsed_s = run_binary(core, rounds)
            if run.returncode != 0:
                print(f"Execution failed for mode {mode_name}, rounds={rounds}")
                if run.stderr:
                    print(run.stderr)
                continue

            correct, total, accuracy, attack_elapsed_ns, attack_elapsed_s, data_rate_bytes_s = parse_attack_metrics(run.stdout)
            if data_rate_bytes_s is None and attack_elapsed_s:
                data_rate_bytes_s = rounds / attack_elapsed_s / 8.0
            raw_output = " ".join(run.stdout.split())
            rows.append([
                mode_name,
                sw,
                rounds,
                correct if correct is not None else "",
                total if total is not None else "",
                accuracy if accuracy is not None else "",
                attack_elapsed_ns if attack_elapsed_ns is not None else "",
                attack_elapsed_s if attack_elapsed_s is not None else "",
                data_rate_bytes_s if data_rate_bytes_s is not None else "",
                process_elapsed_s,
                raw_output,
            ])
            print(run.stdout.strip())

    with open(output_file, "w", encoding="utf-8") as f:
        f.write("mode\tTEST_ON_SW\trounds\tcorrect\ttotal\taccuracy\tattack_elapsed_ns\tattack_elapsed_s\tdata_rate_bytes_s\tprocess_elapsed_s\traw_output\n")
        for row in rows:
            f.write("\t".join(str(item) for item in row) + "\n")

    print(f"Saved timing result to {output_file}")
    summarize_timing(arch, core)
    return output_file


def summarize_timing(arch, core):
    input_file = timing_file(arch, core)
    summary_file = f"res/single-summary-{arch}-core{core}.txt"
    if not os.path.exists(input_file):
        print(f"Error: timing file '{input_file}' not found.")
        return

    groups = {}
    execution_rows = []
    with open(input_file, "r", encoding="utf-8") as f:
        reader = csv.DictReader(f, delimiter="\t")
        for row in reader:
            execution_rows.append(row)
            mode = row["mode"]
            groups.setdefault(mode, {"accuracy": [], "data_rate": []})

            if row.get("accuracy"):
                groups[mode]["accuracy"].append(float(row["accuracy"]))

            if row.get("data_rate_bytes_s"):
                groups[mode]["data_rate"].append(float(row["data_rate_bytes_s"]))
            elif row.get("bandwidth_bps"):
                groups[mode]["data_rate"].append(float(row["bandwidth_bps"]) / 8.0)

    summary = {}
    for mode, values in groups.items():
        avg_accuracy = sum(values["accuracy"]) / len(values["accuracy"]) if values["accuracy"] else None
        avg_data_rate = sum(values["data_rate"]) / len(values["data_rate"]) if values["data_rate"] else None
        summary[mode] = {
            "avg_accuracy": avg_accuracy,
            "avg_data_rate": avg_data_rate,
        }

    comparison = None
    load = summary.get("load")
    sw_prefetch = summary.get("sw_prefetch")
    if load and sw_prefetch:
        load_accuracy = load["avg_accuracy"]
        sw_accuracy = sw_prefetch["avg_accuracy"]
        load_data_rate = load["avg_data_rate"]
        sw_data_rate = sw_prefetch["avg_data_rate"]

        accuracy_delta = (
            sw_accuracy - load_accuracy
            if load_accuracy is not None and sw_accuracy is not None
            else None
        )
        data_rate_delta = (
            sw_data_rate - load_data_rate
            if load_data_rate is not None and sw_data_rate is not None
            else None
        )
        data_rate_change_percent = (
            data_rate_delta / load_data_rate * 100
            if data_rate_delta is not None and load_data_rate
            else None
        )
        comparison = {
            "accuracy_delta": accuracy_delta,
            "data_rate_delta": data_rate_delta,
            "data_rate_change_percent": data_rate_change_percent,
        }

    summary_lines = []
    for mode, values in summary.items():
        avg_accuracy = values["avg_accuracy"]
        avg_data_rate = values["avg_data_rate"]
        accuracy_text = f"{avg_accuracy * 100:.2f}%" if avg_accuracy is not None else "N/A"
        data_rate_text = f"{avg_data_rate:.3f} Bytes/s" if avg_data_rate is not None else "N/A"
        summary_lines.append(f"{mode}: avg_accuracy={accuracy_text}, avg_data_rate={data_rate_text}")
    if comparison:
        accuracy_delta = comparison["accuracy_delta"]
        data_rate_delta = comparison["data_rate_delta"]
        data_rate_change_percent = comparison["data_rate_change_percent"]
        if accuracy_delta is not None:
            summary_lines.append(f"sw_prefetch vs load: avg_accuracy_delta={accuracy_delta * 100:+.2f} percentage points")
        if data_rate_delta is not None and data_rate_change_percent is not None:
            summary_lines.append(
                "sw_prefetch vs load: "
                f"avg_data_rate_delta={data_rate_delta:+.3f} Bytes/s, "
                f"avg_data_rate_change={data_rate_change_percent:+.2f}%"
            )

    with open(summary_file, "w", encoding="utf-8") as f:
        f.write("mode\tavg_accuracy\tavg_accuracy_percent\tavg_data_rate_bytes_s\n")
        for mode, values in summary.items():
            avg_accuracy = values["avg_accuracy"] if values["avg_accuracy"] is not None else ""
            avg_data_rate = values["avg_data_rate"] if values["avg_data_rate"] is not None else ""
            avg_accuracy_percent = avg_accuracy * 100 if avg_accuracy != "" else ""
            f.write(f"{mode}\t{avg_accuracy}\t{avg_accuracy_percent}\t{avg_data_rate}\n")
        if comparison:
            f.write("\ncomparison\tvalue\n")
            if comparison["accuracy_delta"] is not None:
                f.write(f"sw_vs_load_accuracy_delta_percent_point\t{comparison['accuracy_delta'] * 100}\n")
            if comparison["data_rate_delta"] is not None:
                f.write(f"sw_vs_load_data_rate_delta_bytes_s\t{comparison['data_rate_delta']}\n")
            if comparison["data_rate_change_percent"] is not None:
                f.write(f"sw_vs_load_data_rate_change_percent\t{comparison['data_rate_change_percent']}\n")

        if summary_lines:
            f.write("\ntext_summary\n")
            for line in summary_lines:
                f.write(f"{line}\n")

        if execution_rows:
            f.write("\nexecutions\n")
            f.write("mode\tTEST_ON_SW\trounds\tcorrect\ttotal\taccuracy\tattack_elapsed_ns\tattack_elapsed_s\tdata_rate_bytes_s\tprocess_elapsed_s\traw_output\n")
            for row in execution_rows:
                f.write(
                    "\t".join(
                        row.get(column, "")
                        for column in [
                            "mode",
                            "TEST_ON_SW",
                            "rounds",
                            "correct",
                            "total",
                            "accuracy",
                            "attack_elapsed_ns",
                            "attack_elapsed_s",
                            "data_rate_bytes_s",
                            "process_elapsed_s",
                        ]
                    )
                    + "\t"
                    + row_raw_output(row)
                    + "\n"
                )

    print(f"Saved timing summary to {summary_file}")
    for line in summary_lines:
        print(line)


def parse_rounds_list(value):
    rounds = []
    for item in value.split(","):
        item = item.strip()
        if not item:
            continue
        rounds.append(int(item))
    if not rounds:
        raise argparse.ArgumentTypeError("rounds list cannot be empty")
    if any(round <= 0 for round in rounds):
        raise argparse.ArgumentTypeError("all rounds must be positive")
    return rounds


def plot_timing(arch, core):
    import pandas as pd
    import seaborn as sns
    import matplotlib.pyplot as plt

    input_file = timing_file(arch, core)
    if not os.path.exists(input_file):
        print(f"Error: timing file '{input_file}' not found.")
        return

    print(f"Reading timing data from {input_file}...")
    try:
        df = pd.read_csv(input_file, sep="\t")
    except Exception as e:
        print(f"Failed to open timing file: {e}")
        return

    if df.empty:
        print("No timing data found.")
        return

    if "data_rate_bytes_s" not in df.columns and "bandwidth_bps" in df.columns:
        df["data_rate_bytes_s"] = df["bandwidth_bps"] / 8.0

    df["run_id"] = df.groupby(["mode", "rounds"]).cumcount() + 1
    df["round_label"] = df.apply(
        lambda row: f"{row['rounds']}#{row['run_id']}" if row["run_id"] > 1 else str(row["rounds"]),
        axis=1,
    )

    fig, axes = plt.subplots(1, 2, figsize=(12, 4.8))
    sns.barplot(data=df, x="round_label", y="attack_elapsed_s", hue="mode", ax=axes[0])
    axes[0].set_title("Attack Time")
    axes[0].set_xlabel("Leak Bits")
    axes[0].set_ylabel("Seconds")

    sns.barplot(data=df, x="round_label", y="data_rate_bytes_s", hue="mode", ax=axes[1])
    axes[1].set_title("Data Rate")
    axes[1].set_xlabel("Leak Bits")
    axes[1].set_ylabel("Bytes/s")

    for ax in axes:
        ax.grid(axis="y", linestyle="--", alpha=0.35)

    plt.tight_layout()
    os.makedirs("res/plots", exist_ok=True)
    output_path = os.path.join("res/plots", f"timing-{arch}-core{core}.png")
    plt.savefig(output_path, dpi=300)
    plt.close()
    print(f"Saved timing plot to {output_path}")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(
        description="Benchmark single-bit attack time and data rate."
    )
    parser.add_argument(
        "--no-plot",
        action="store_true",
        help="Only run benchmark and save timing results, do not draw timing plot.",
    )
    parser.add_argument(
        "--arch",
        default=DEFAULT_ARCH,
        help=f"Architecture name used in result filenames. Default: {DEFAULT_ARCH}.",
    )
    parser.add_argument(
        "--core",
        type=int,
        default=DEFAULT_CORE,
        help=f"CPU core id used by taskset. Default: {DEFAULT_CORE}.",
    )
    parser.add_argument(
        "--benchmark-rounds",
        type=parse_rounds_list,
        default=DEFAULT_BENCHMARK_ROUNDS,
        help="Comma-separated rounds list. Default: 100,200,400,600,800,1000.",
    )
    args = parser.parse_args()

    benchmark_time(args.arch, args.core, args.benchmark_rounds)
    if not args.no_plot:
        plot_timing(args.arch, args.core)
