#!/usr/bin/env python3
"""Run fixed-thread cross-core BOP controls in independent processes."""
import argparse
import csv
import hashlib
import io
import json
from pathlib import Path
import random
import subprocess

HERE = Path(__file__).resolve().parent
CASES = ("untrained", "local_n", "local_nm", "remote_same", "remote_other",
         "remote_only", "no_trigger")


def topology(cpus):
    result = {}
    for cpu in sorted(set(cpus)):
        root = Path(f"/sys/devices/system/cpu/cpu{cpu}")
        midr = int((root / "regs/identification/midr_el1").read_text(), 16)
        cache = []
        for index in sorted((root / "cache").glob("index*")):
            cache.append({key: (index / key).read_text().strip()
                          for key in ("level", "type", "size", "shared_cpu_list")})
        result[cpu] = dict(midr=hex(midr), part=hex((midr >> 4) & 0xfff), cache=cache)
    return result


def aggregate(rows):
    keys = sorted({(r["cpu_a"], r["cpu_b"], r["n"], r["m"], r["case"], r["measure"])
                   for r in rows})
    result = []
    for key in keys:
        group = [r for r in rows if tuple(r[k] for k in
                 ("cpu_a", "cpu_b", "n", "m", "case", "measure")) == key]
        item = dict(zip(("cpu_a", "cpu_b", "n", "m", "case", "measure"), key))
        values = [float(r["hit_pct"]) for r in group]
        item.update(repeats=len(group), hit_pct_min=min(values), hit_pct_max=max(values))
        for name in ("hit_pct", "avg_ns", "a_event0_per_load", "a_event1_per_load",
                     "b_event0_per_load", "b_event1_per_load", "b_train_us", "phase_us"):
            item[name + "_mean"] = sum(float(r[name]) for r in group) / len(group)
        item["remote_overruns"] = sum(int(r["remote_overruns"]) for r in group)
        item["local_overruns"] = sum(int(r["local_overruns"]) for r in group)
        result.append(item)
    return result


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--binary", type=Path, default=HERE / "test-bop-cross-core")
    p.add_argument("--build", action="store_true")
    p.add_argument("--directions", default="6:7,7:6,6:16,16:6")
    p.add_argument("--train-a-pairs", type=int, default=300)
    p.add_argument("--train-b-pairs", default="300,1500,3000")
    p.add_argument("--offset-lines", type=int, default=16)
    p.add_argument("--other-offset", type=int, default=13)
    p.add_argument("--rounds", type=int, default=30)
    p.add_argument("--repeats", type=int, default=3)
    p.add_argument("--hold-us", type=int, default=2000)
    p.add_argument("--scrub-loads", type=int, default=4096)
    p.add_argument("--seed", type=int, default=20260913)
    p.add_argument("--cases", default=",".join(CASES))
    p.add_argument("--measures", default="cache,demand")
    p.add_argument("--output", required=True, type=Path)
    args = p.parse_args()
    try:
        directions = [tuple(map(int, entry.split(":"))) for entry in args.directions.split(",")]
        amounts = [int(n) for n in args.train_b_pairs.split(",")]
        if any(len(pair) != 2 or pair[0] == pair[1] for pair in directions):
            raise ValueError("directions must have two distinct CPUs")
        if any(n < 0 or n > 4096 for n in amounts) or not 0 <= args.train_a_pairs <= 4096:
            raise ValueError("pair counts must be in 0..4096")
    except ValueError as exc:
        p.error(str(exc))
    if args.repeats < 1 or args.rounds < 1:
        p.error("rounds and repeats must be positive")
    cases = args.cases.split(",")
    measures = args.measures.split(",")
    if not set(cases) <= set(CASES) or not set(measures) <= {"cache", "demand", "prefetch"}:
        p.error("unknown case or measure")
    if args.output.exists() or args.output.with_suffix(".json").exists():
        p.error("output already exists; choose a new path")
    module_loaded = Path("/sys/module/prefetch_pmu_event").exists()
    if module_loaded and set(measures) != {"cache"}:
        p.error("unload prefetch_pmu_event before perf measurements; restore it afterwards")
    topo = topology([cpu for pair in directions for cpu in pair])
    if any(info["part"] != "0xd85" for info in topo.values()):
        p.error("this experiment is calibrated for X925 (MIDR part 0xd85)")
    if args.build:
        subprocess.run(["gcc", "-std=gnu11", "-O2", "-Wall", "-Wextra", "-Werror",
                        "-march=armv8.5-a+predres", "-pthread", str(HERE / "test-bop-cross-core.c"),
                        "-o", str(args.binary)], check=True)
    jobs = [(a, b, m, case) for a, b in directions for m in amounts for case in cases]
    args.output.parent.mkdir(parents=True, exist_ok=True)
    metadata = {key: str(value) if isinstance(value, Path) else value
                for key, value in vars(args).items()}
    metadata.update(topology=topo, pmu_module_loaded=module_loaded,
                    binary_sha256=hashlib.sha256(args.binary.read_bytes()).hexdigest(),
                    source_sha256=hashlib.sha256((HERE / "test-bop-cross-core.c").read_bytes()).hexdigest(),
                    runner_sha256=hashlib.sha256(Path(__file__).read_bytes()).hexdigest())
    args.output.with_suffix(".json").write_text(json.dumps(metadata, indent=2) + "\n")
    rows = []
    with args.output.open("x", newline="") as stream:
        writer = None
        for measure in measures:
            for repeat in range(args.repeats):
                order = jobs.copy()
                random.Random(args.seed + repeat).shuffle(order)
                for index, (a, b, m, case) in enumerate(order):
                    cmd = [str(args.binary), "--cpu-a", str(a), "--cpu-b", str(b),
                           "--train-a-pairs", str(args.train_a_pairs), "--train-b-pairs", str(m),
                           "--offset-lines", str(args.offset_lines), "--other-offset", str(args.other_offset),
                           "--rounds", str(args.rounds), "--case", case, "--measure", measure,
                           "--hold-us", str(args.hold_us), "--scrub-loads", str(args.scrub_loads),
                           "--seed", str(args.seed + repeat)]
                    run = subprocess.run(cmd, text=True, capture_output=True, timeout=120)
                    if run.returncode:
                        raise RuntimeError(f"{cmd}\nexit={run.returncode}\n{run.stderr}")
                    data = list(csv.DictReader(io.StringIO(run.stdout), delimiter="\t"))
                    if len(data) != 1:
                        raise RuntimeError(f"invalid result: {run.stdout}")
                    row = dict(repeat=repeat + 1, **data[0])
                    if row["observed_a"] != str(a) or row["observed_b"] != str(b):
                        raise RuntimeError("CPU binding mismatch")
                    if writer is None:
                        writer = csv.DictWriter(stream, fieldnames=list(row), delimiter="\t")
                        writer.writeheader()
                    writer.writerow(row); stream.flush(); rows.append(row)
                    if (index + 1) % 28 == 0 or index + 1 == len(order):
                        print(f"{measure}: repeat {repeat+1}/{args.repeats}, {index+1}/{len(order)}", flush=True)
    summary = aggregate(rows)
    output = args.output.with_name(args.output.stem + "-summary.tsv")
    with output.open("w", newline="") as stream:
        writer = csv.DictWriter(stream, fieldnames=list(summary[0]), delimiter="\t")
        writer.writeheader(); writer.writerows(summary)
    print(output)


if __name__ == "__main__":
    main()
