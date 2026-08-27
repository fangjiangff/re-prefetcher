#!/usr/bin/env python3

import argparse
import re
import subprocess
import sys
from pathlib import Path


HERE = Path(__file__).resolve().parent
RUNNER = HERE / "test-onArray2.py"
RES_DIR = HERE / "res"

ROW_RE = re.compile(
    r"array_index=\s*(?P<idx>\d+),\s*"
    r"offset_bytes=\s*(?P<pos>\d+) \* LINE_SIZE,\s*"
    r"avg_ns=\s*(?P<lat>-?\d+),\s*"
    r"probes=\s*(?P<probes>\d+)"
)


def parse_ranges(spec):
    ranges = []
    if not spec:
        return ranges

    for item in spec.split(","):
        item = item.strip()
        if not item:
            continue
        if ":" not in item:
            raise SystemExit(f"invalid range {item!r}; expected START:END")
        start_s, end_s = item.split(":", 1)
        start = int(start_s, 0)
        end = int(end_s, 0)
        if start < 0 or end <= start or end > 256:
            raise SystemExit(f"invalid range {item!r}; expected 0 <= START < END <= 256")
        ranges.append((start, end))
    return ranges


def make_sweep_ranges(args):
    ranges = parse_ranges(args.ranges)
    if ranges:
        return ranges

    if args.sweep_start < 0 or args.sweep_stop > 256 or args.width <= 0 or args.step <= 0:
        raise SystemExit("sweep bounds must satisfy 0 <= start, stop <= 256, width > 0, step > 0")

    for start in range(args.sweep_start, args.sweep_stop + 1, args.step):
        end = start + args.width
        if end <= 256:
            ranges.append((start, end))
    if not ranges:
        raise SystemExit("no trigger ranges generated")
    return ranges


def parse_result(path):
    rows = []
    for line in path.read_text().splitlines():
        m = ROW_RE.search(line)
        if not m:
            continue
        rows.append({
            "idx": int(m.group("idx")),
            "pos": int(m.group("pos")),
            "lat": int(m.group("lat")),
            "probes": int(m.group("probes")),
        })
    return rows


def analyze_rows(rows, start, end, threshold_ns, min_probes):
    direct_hits = []
    outside_hits = []
    outside_fastest = []

    for row in rows:
        if row["probes"] < min_probes or row["lat"] < 0:
            continue
        is_hit = row["lat"] <= threshold_ns
        in_trigger = start <= row["idx"] < end
        if is_hit and in_trigger:
            direct_hits.append(row)
        elif is_hit:
            outside_hits.append(row)
        if not in_trigger:
            outside_fastest.append(row)

    outside_fastest.sort(key=lambda row: row["lat"])
    outside_hits.sort(key=lambda row: row["idx"])

    return {
        "trigger_hits": direct_hits,
        "outside_hits": outside_hits,
        "outside_fastest": outside_fastest[:8],
        "first_outside_hit": outside_hits[0] if outside_hits else None,
        "prefetch": bool(outside_hits),
    }


def format_hits(rows, limit=10):
    shown = rows[:limit]
    text = ",".join(f"{row['idx']}:{row['lat']}" for row in shown)
    if len(rows) > limit:
        text += f",+{len(rows) - limit}"
    return text


def run_one(args, start, end):
    stem = (
        f"sweep-onArray2-{args.arch}-cpu{args.core if args.core is not None else 'auto'}-"
        f"window-r{args.rounds}-p{args.probe_positions}-tw{start}-{end}-"
        f"thr{args.threshold_ns}-csw{1 if args.context_switch_flush else 0}"
    )
    output = RES_DIR / f"{stem}.txt"

    cmd = [
        sys.executable,
        str(RUNNER),
        "--arch",
        args.arch,
        "--mode",
        "window",
        "--rounds",
        str(args.rounds),
        "--probe-positions",
        str(args.probe_positions),
        "--trigger-start",
        str(start),
        "--trigger-end",
        str(end),
        "--random-pages",
        str(args.random_pages),
        "--random-pcs",
        str(args.random_pcs),
        "--output",
        str(output),
        "--no-plot",
    ]
    if args.core is not None:
        cmd.extend(["--core", str(args.core)])
    if args.context_switch_flush:
        cmd.append("--context-switch-flush")
    if args.no_static:
        cmd.append("--no-static")
    if args.force_run:
        cmd.append("--force-run")
    if args.runner:
        cmd.extend(["--runner", args.runner])

    print("+", " ".join(cmd), flush=True)
    if not args.analyze_only:
        result = subprocess.run(cmd, text=True, capture_output=True)
        if result.stdout:
            print(result.stdout, end="")
        if result.stderr:
            print(result.stderr, end="", file=sys.stderr)
        if result.returncode != 0:
            raise SystemExit(result.returncode)
    elif not output.exists():
        raise SystemExit(f"missing result for --analyze-only: {output}")

    rows = parse_result(output)
    analysis = analyze_rows(rows, start, end, args.threshold_ns, args.min_probes)
    return output, rows, analysis


def main():
    parser = argparse.ArgumentParser(
        description="Sweep test-onArray2 trigger windows and flag cache hits outside the direct-load window."
    )
    parser.add_argument("--ranges", default=None,
                        help="Comma-separated trigger windows, for example 150:175,160:185,170:195.")
    parser.add_argument("--sweep-start", type=int, default=120)
    parser.add_argument("--sweep-stop", type=int, default=220)
    parser.add_argument("--width", type=int, default=25)
    parser.add_argument("--step", type=int, default=10)
    parser.add_argument("--threshold-ns", type=int, default=120)
    parser.add_argument("--min-probes", type=int, default=1)
    parser.add_argument("--rounds", type=int, default=80000)
    parser.add_argument("--probe-positions", type=int, default=2000)
    parser.add_argument("--random-pages", type=int, default=1024)
    parser.add_argument("--random-pcs", type=int, default=8)
    parser.add_argument("-a", "--arch", type=str.upper, default="A78")
    parser.add_argument("-c", "--core", type=int, default=None)
    parser.add_argument("--context-switch-flush", action="store_true")
    parser.add_argument("--runner", default=None)
    parser.add_argument("--force-run", action="store_true")
    parser.add_argument("--no-static", action="store_true")
    parser.add_argument("--analyze-only", action="store_true")
    parser.add_argument("--report", default=None)
    args = parser.parse_args()

    ranges = make_sweep_ranges(args)
    RES_DIR.mkdir(exist_ok=True)

    report = Path(args.report) if args.report else RES_DIR / (
        f"sweep-onArray2-{args.arch}-window-r{args.rounds}-p{args.probe_positions}-"
        f"thr{args.threshold_ns}-w{args.width}-s{args.step}.tsv"
    )
    report.parent.mkdir(parents=True, exist_ok=True)

    lines = [
        "start\tend\tprefetch\tfirst_hit_idx\tfirst_hit_pos\tfirst_hit_lat_ns\t"
        "outside_hit_count\ttrigger_hit_count\toutside_hits_idx_lat\tfastest_outside_idx_lat\tresult"
    ]

    for start, end in ranges:
        output, rows, analysis = run_one(args, start, end)
        first = analysis["first_outside_hit"]
        line = "\t".join([
            str(start),
            str(end),
            "yes" if analysis["prefetch"] else "no",
            str(first["idx"]) if first else "-",
            str(first["pos"]) if first else "-",
            str(first["lat"]) if first else "-",
            str(len(analysis["outside_hits"])),
            str(len(analysis["trigger_hits"])),
            format_hits(analysis["outside_hits"]),
            format_hits(analysis["outside_fastest"]),
            str(output),
        ])
        lines.append(line)
        print(line)

    report.write_text("\n".join(lines) + "\n")
    print(f"Saved sweep report to {report}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
