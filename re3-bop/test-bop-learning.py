#!/usr/bin/env python3
"""Matched-load learning experiments; independent, shuffled child processes."""
import argparse
import csv
import hashlib
import io
import json
from pathlib import Path
import random
import subprocess
import sys

HERE = Path(__file__).resolve().parent


def cases():
    out = []

    def add(name, **kw):
        base = dict(pattern="pair", pc="single", flush="pair",
                    initial_pairs=300, loads=600, pages=4096)
        base.update(kw)
        out.append((name, base))

    add("baseline_a300", loads=0)
    add("baseline_a0", initial_pairs=0, loads=0)
    add("positive_a1000", initial_pairs=1000, loads=0)
    add("random_b600", pattern="random")
    add("no_trigger_pair", no_trigger=True)
    add("off_target_1_pair", probe_offset=1)
    add("off_target_32_pair", probe_offset=32)
    for pattern in ("stride", "pair", "chain"):
        for flush in ("batch", "pair", "each", "fence"):
            add(f"{pattern}_{flush}", pattern=pattern, flush=flush)
        for pc in ("rotate", "random"):
            add(f"{pattern}_batch_{pc}", pattern=pattern, flush="batch", pc=pc)
    for pages in (1, 4, 16, 64):
        add(f"pair_pages{pages}", pages=pages)
        add(f"pair_pages{pages}_a0", pages=pages, initial_pairs=0, loads=2000)
    for loads in (300, 1200, 2400):
        add(f"stride_batch_n{loads}", pattern="stride", flush="batch", loads=loads)
        add(f"stride_each_n{loads}", pattern="stride", flush="each", loads=loads)
    add("stride_each_a0", pattern="stride", flush="each", initial_pairs=0, loads=2000)
    add("stride_batch_a0", pattern="stride", flush="batch", initial_pairs=0, loads=2000)
    add("stride_each_no_trigger", pattern="stride", flush="each", no_trigger=True)
    for probe in (1, 32):
        add(f"stride_batch_probe{probe}", pattern="stride", flush="batch", probe_offset=probe)
        add(f"stride_each_probe{probe}", pattern="stride", flush="each", probe_offset=probe)
    for gap in (0, 10, 1000):
        add(f"stride_batch_gap{gap}", pattern="stride", flush="batch", gap=gap)
    return out


def residency_cases():
    out = []

    def add(name, **kw):
        cfg = dict(pattern="pair", pc="single", flush="batch", initial_pairs=300,
                   loads=600, pages=4096, resident="cold", evict_kb=256)
        cfg.update(kw)
        out.append((name, cfg))

    add("cold")
    add("cold_skip", skip_b=True)
    add("cold_each", flush="each")
    add("cold_no_trigger", no_trigger=True)
    for resident in ("demand", "swl1", "swl2", "hw-stream", "hw-offset",
                     "hw-offset-consumed", "hw-offset-demand"):
        add(resident, resident=resident)
        add(resident + "_skip", resident=resident, skip_b=True)
        add(resident + "_rescue", resident=resident, flush="each")
        add(resident + "_n2000", resident=resident, loads=2000)
        add(resident + "_a0", resident=resident, initial_pairs=0, loads=2000)
        for kb in (0, 128, 512):
            add(resident + f"_evict{kb}", resident=resident, evict_kb=kb)
    add("hw-offset_shuffle", resident="hw-offset", shuffle_b=True)
    add("hw-offset_no_trigger", resident="hw-offset", no_trigger=True)
    for limit in (16, 32, 64, 128):
        add(f"hw-offset_cold{limit}", resident="hw-offset", flush="each", b_limit=limit)
    return out


def command(binary, config, measure, seed, args):
    cmd = [str(binary), "--cpu", str(args.cpu), "--rounds", str(args.rounds),
           "--offset", str(args.offset), "--measure", measure, "--seed", str(seed)]
    for key, value in config.items():
        if isinstance(value, bool):
            if value:
                cmd.append("--" + key.replace("_", "-"))
        else:
            cmd += ["--" + key.replace("_", "-"), str(value)]
    return cmd


def summarize(rows):
    keys = sorted({(r["case"], r["measure"]) for r in rows})
    summary = []
    for case, measure in keys:
        group = [r for r in rows if (r["case"], r["measure"]) == (case, measure)]
        values = [float(r["hit_pct"]) for r in group]
        item = dict(case=case, measure=measure, repeats=len(group),
                    hit_pct_min=min(values), hit_pct_max=max(values))
        for name in ("hit_pct", "avg_ns", "train_hit_pct", "train_avg_ns",
                     "l1_per_load", "l2_per_load", "l1_miss_per_load", "l2_miss_per_load"):
            item[name + "_mean"] = sum(float(r[name]) for r in group) / len(group)
        for name in ("event0_per_load", "event1_per_load"):
            if name in group[0]:
                item[name + "_mean"] = sum(float(r[name]) for r in group) / len(group)
        summary.append(item)
    return summary


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, default=HERE / "test-bop-learning")
    parser.add_argument("--build", action="store_true")
    parser.add_argument("--cpu", type=int, default=6)
    parser.add_argument("--offset", type=int, default=16)
    parser.add_argument("--rounds", type=int, default=30)
    parser.add_argument("--repeats", type=int, default=3)
    parser.add_argument("--seed", type=int, default=20260912)
    parser.add_argument("--cases", help="Comma-separated case names")
    parser.add_argument("--probe-scan", action="store_true",
                        help="Probe +1..+32 in separate processes for each selected case")
    parser.add_argument("--suite", choices=("learning", "residency"), default="learning")
    parser.add_argument("--measures", default="prefetch,cache,timing,demand,access")
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    if args.repeats < 1 or args.rounds < 1:
        parser.error("rounds and repeats must be positive")
    selected = residency_cases() if args.suite == "residency" else cases()
    if args.cases:
        wanted = set(args.cases.split(","))
        if wanted - {name for name, _ in selected}:
            parser.error("unknown case")
        selected = [(name, cfg) for name, cfg in selected if name in wanted]
    if args.probe_scan:
        selected = [(f"{name}_scan{probe}", dict(cfg, probe_offset=probe))
                    for name, cfg in selected for probe in range(1, 33)]
    measures = args.measures.split(",")
    if not set(measures) <= {"pmu", "cache", "timing", "demand", "access", "prefetch", "supply"}:
        parser.error("unknown measure")
    if args.build:
        subprocess.run(["gcc", "-std=gnu11", "-O2", "-Wall", "-Wextra", "-Werror",
                        "-march=armv8.5-a+predres", str(HERE / "test-bop-learning.c"),
                        "-o", str(args.binary)], check=True)
    if args.output.exists():
        parser.error("output already exists; choose a new path")
    args.output.parent.mkdir(parents=True, exist_ok=True)
    meta = dict(cpu=args.cpu, offset=args.offset, rounds=args.rounds,
                repeats=args.repeats, seed=args.seed, cases=selected, measures=measures,
                binary_sha256=hashlib.sha256(args.binary.read_bytes()).hexdigest(),
                source_sha256=hashlib.sha256((HERE / "test-bop-learning.c").read_bytes()).hexdigest())
    meta["pmu_module_loaded"] = Path("/sys/module/prefetch_pmu_event").exists()
    meta["suite"] = args.suite
    args.output.with_suffix(".json").write_text(json.dumps(meta, indent=2) + "\n")
    rows = []
    with args.output.open("x", newline="") as stream:
        writer = None
        for measure in measures:
            for repeat in range(args.repeats):
                order = selected.copy()
                random.Random(args.seed + repeat).shuffle(order)
                for name, cfg in order:
                    cmd = command(args.binary, cfg, measure, args.seed + repeat, args)
                    run = subprocess.run(cmd, text=True, capture_output=True)
                    if run.returncode:
                        sys.exit(f"{cmd}\nexit={run.returncode}\n{run.stderr}")
                    records = list(csv.DictReader(io.StringIO(run.stdout), delimiter="\t"))
                    if len(records) != 1:
                        sys.exit(f"invalid result: {run.stdout}")
                    row = dict(case=name, repeat=repeat + 1, **records[0])
                    if writer is None:
                        writer = csv.DictWriter(stream, fieldnames=list(row), delimiter="\t")
                        writer.writeheader()
                    writer.writerow(row)
                    stream.flush()
                    rows.append(row)
                print(f"{measure} repeat {repeat+1}/{args.repeats}: {len(order)} cases", flush=True)
    summary = summarize(rows)
    path = args.output.with_name(args.output.stem + "-summary.tsv")
    with path.open("w", newline="") as stream:
        writer = csv.DictWriter(stream, fieldnames=list(summary[0]), delimiter="\t")
        writer.writeheader()
        writer.writerows(summary)
    print(path)


if __name__ == "__main__":
    main()
