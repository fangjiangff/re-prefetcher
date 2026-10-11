"""Single-agent campaign for reverse-engineering Load/Store stride prefetchers."""

from __future__ import annotations

import argparse
import csv
import hashlib
import json
import os
import platform
import re
import statistics
import subprocess
import sys
from datetime import datetime, timezone
from pathlib import Path
from typing import Any


REPO = Path(__file__).resolve().parent.parent
STRIDE_ROOT = REPO / "re0-stride-prefetcher"
TRIGGER = STRIDE_ROOT / "test0-trigger" / "test0-trigger.py"
DEGREE = STRIDE_ROOT / "test0-trigger" / "test0-degree.py"
INDEX_MODE = STRIDE_ROOT / "test1-index" / "test1-pmu-index-mode.py"
INDEX_PC = STRIDE_ROOT / "test1-index" / "test1-index-pc.py"
INDEX_VA_XOR = STRIDE_ROOT / "test1-index" / "test1-index-mem-va-xor.py"
INDEX_PA_XOR = STRIDE_ROOT / "test1-index" / "test1-index-mem-pa-xor.py"
ENTRY_NUMS = STRIDE_ROOT / "test2-entry" / "test2-nums.py"
REPLACEMENT = STRIDE_ROOT / "test2-entry" / "test2-rep-policy.py"
CROSS_CONTEXT = STRIDE_ROOT / "run-all-cross-tests.py"

ANSI_RE = re.compile(r"\x1b\[[0-9;]*m")
PMU_THRESHOLD = 0.5
ACCESS_TYPES = ("load", "store")
PHASES = ("existence", "range", "degree", "index", "capacity", "context")


class CampaignError(RuntimeError):
    pass


def utc_now() -> str:
    return datetime.now(timezone.utc).isoformat()


def clean_ansi(value: str) -> str:
    return ANSI_RE.sub("", value)


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for block in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def read_tsv(path: Path) -> list[dict[str, str]]:
    with path.open(newline="") as handle:
        return list(csv.DictReader(handle, delimiter="\t"))


def median(values: list[float]) -> float | None:
    return statistics.median(values) if values else None


def first_cache_activation(rows: list[dict[str, str]], threshold: int) -> int | None:
    positives = [
        int(row["train_step"])
        for row in rows
        if float(row["avg_ns"]) <= threshold
    ]
    return min(positives) if positives else None


def pmu_by_step(paths: list[Path], stride: int) -> dict[int, list[float]]:
    values: dict[int, list[float]] = {}
    for path in paths:
        for row in read_tsv(path):
            if int(row["stride_lines"]) != stride:
                continue
            values.setdefault(int(row["train_step"]), []).append(
                float(row["pmu_per_round"])
            )
    return values


def first_replicated_pmu_activation(
    values: dict[int, list[float]], threshold: float = PMU_THRESHOLD,
) -> int | None:
    for step in sorted(values):
        samples = values[step]
        required = max(1, (len(samples) + 1) // 2)
        if sum(value >= threshold for value in samples) >= required:
            return step
    return None


def first_stable_pmu_activation(
    values: dict[int, list[float]], threshold: float = PMU_THRESHOLD,
) -> int | None:
    for step in sorted(values):
        samples = values[step]
        if samples and all(value >= threshold for value in samples):
            return step
    return None


def parse_degree(path: Path, train_step: int, threshold: int) -> dict[str, Any]:
    selected = [
        row for row in read_tsv(path)
        if int(row["train_step"]) == train_step and row["role"] == "candidate"
    ]
    selected.sort(key=lambda row: int(row["probe_pos"]))
    cached_positions = [
        int(row["probe_pos"]) for row in selected
        if int(row["avg_ns"]) <= threshold
    ]
    consecutive = 0
    expected = train_step
    for position in cached_positions:
        if position < expected:
            continue
        if position != expected:
            break
        consecutive += 1
        expected += 1
    return {
        "train_step": train_step,
        "cached_candidate_positions": cached_positions,
        "consecutive_degree": consecutive,
        "all_cached_candidate_count": len(cached_positions),
    }


def parse_index_mode(text: str) -> dict[str, Any]:
    rows: dict[str, dict[str, Any]] = {}
    for raw in clean_ansi(text).splitlines():
        fields = raw.split()
        if not fields or fields[0] not in {
            "T0_no_trigger", "T0", "T1", "T2", "T3", "T4"
        }:
            continue
        if len(fields) < 3:
            continue
        rows[fields[0]] = {
            "latency": int(fields[1]),
            "prefetched": fields[2] == "yes",
            "counters": fields[3:],
        }
    required = {"T0_no_trigger", "T0", "T1", "T2", "T3", "T4"}
    sanity = (
        required.issubset(rows)
        and not rows["T0_no_trigger"]["prefetched"]
        and rows["T0"]["prefetched"]
    )
    result: dict[str, Any] = {"rows": rows, "sanity_passed": sanity}
    if not sanity:
        result.update({
            "state": "inconclusive",
            "reason": "T0_no_trigger/T0 sanity gate failed or rows are missing",
        })
        return result
    result.update({
        "state": "observed",
        "pc_equality_required_candidate": not rows["T1"]["prefetched"],
        "va_equality_required_candidate": not rows["T2"]["prefetched"],
        "same_va_different_pa_cross_process": rows["T3"]["prefetched"],
        "pa_sufficient_candidate": rows["T4"]["prefetched"],
        "limitations": [
            "T3 changes both PA and process context",
            "a failed equality test identifies match/index/tag sensitivity, not an index bit alone",
        ],
    })
    return result


def parse_pc_bits(path: Path) -> dict[str, Any]:
    rows = read_tsv(path)
    by_case = {row["case"]: row for row in rows}
    same = by_case.get("same_pc")
    if same is None or same.get("prefetched") != "yes":
        return {"state": "inconclusive", "reason": "same_pc positive control failed"}
    sensitive = []
    insensitive = []
    unmapped = []
    for name, row in by_case.items():
        if not name.startswith("bit_"):
            continue
        bit = int(name.split("_", 1)[1])
        if int(row["latency_ns"]) < 0:
            unmapped.append(bit)
        elif row.get("prefetched") == "yes":
            insensitive.append(bit)
        else:
            sensitive.append(bit)
    return {
        "state": "observed",
        "sensitive_bits": sensitive,
        "insensitive_bits": insensitive,
        "unmapped_bits": unmapped,
        "meaning": "sensitive bits participate in PC match/index/tag/hash input",
        "pairwise_hash_state": "not_implemented_by_existing_PC_benchmark",
    }


class StrideCampaign:
    def __init__(self, args: argparse.Namespace):
        self.args = args
        self.output = args.output.resolve()
        self.logs = self.output / "logs"
        self.state_path = self.output / "campaign.json"
        self.output.mkdir(parents=True, exist_ok=True)
        self.logs.mkdir(parents=True, exist_ok=True)
        if self.state_path.is_file() and args.resume:
            self.state = json.loads(self.state_path.read_text())
        else:
            self.state = {
                "schema_version": "1.0",
                "campaign_id": self.output.name,
                "created_at": utc_now(),
                "updated_at": utc_now(),
                "target": {
                    "arch": args.arch, "core": args.core,
                    "hostname": platform.node(), "machine": platform.machine(),
                },
                "policy": {
                    "access_types": list(ACCESS_TYPES),
                    "stride_lines_for_discovery": args.discovery_stride,
                    "pmu_threshold_per_round": args.pmu_threshold,
                    "cache_hit_threshold": args.hit_threshold,
                    "replicates": args.replicates,
                    "principles": [
                        "cache probe validates PMU existence",
                        "sanity failures block structural inference",
                        "index/match sensitivity is not automatically called an index bit",
                    ],
                },
                "phases": {}, "runs": [], "warnings": [],
            }
            self.save()

    def save(self) -> None:
        self.state["updated_at"] = utc_now()
        self.state_path.write_text(json.dumps(self.state, indent=2) + "\n")

    def run(self, name: str, argv: list[str], *, sudo: bool = False,
            outputs: list[Path] | None = None) -> subprocess.CompletedProcess[str] | None:
        command = (["sudo"] if sudo else []) + argv
        record: dict[str, Any] = {
            "name": name, "argv": command, "started_at": utc_now(),
            "privileged": sudo,
        }
        self.state["runs"].append(record)
        self.save()
        print("# plan:", " ".join(command), flush=True)
        if not self.args.execute:
            record.update({"status": "planned", "finished_at": utc_now()})
            self.save()
            return None
        completed = subprocess.run(
            command, cwd=REPO, text=True, stdout=subprocess.PIPE,
            stderr=subprocess.PIPE, check=False,
        )
        stdout_path = self.logs / f"{name}.stdout.txt"
        stderr_path = self.logs / f"{name}.stderr.txt"
        stdout_path.write_text(completed.stdout)
        stderr_path.write_text(completed.stderr)
        record.update({
            "status": "completed" if completed.returncode == 0 else "failed",
            "return_code": completed.returncode,
            "finished_at": utc_now(),
            "stdout": str(stdout_path.relative_to(self.output)),
            "stderr": str(stderr_path.relative_to(self.output)),
        })
        artifacts = []
        for path in outputs or []:
            if path.is_file():
                artifacts.append({
                    "path": str(path.relative_to(self.output)),
                    "size": path.stat().st_size, "sha256": sha256(path),
                })
        record["artifacts"] = artifacts
        self.save()
        if completed.returncode != 0:
            raise CampaignError(
                f"{name} failed with {completed.returncode}; see {stderr_path}"
            )
        return completed

    def trigger_sweep(
        self, name: str, access: str, min_stride: int, max_stride: int,
        min_step: int, max_step: int, rounds: int, pmu: bool,
    ) -> Path:
        directory = self.output / ("pmu" if pmu else "cache")
        directory.mkdir(exist_ok=True)
        result = directory / f"{name}.tsv"
        raw = directory / f"{name}.raw.txt"
        if self.args.resume and result.is_file():
            return result
        argv = [
            sys.executable, str(TRIGGER), "--arch", self.args.arch,
            "--core", str(self.args.core), "--access", access,
            "--min-stride", str(min_stride), "--max-stride", str(max_stride),
            "--min-step", str(min_step), "--max-step", str(max_step),
            "--rounds", str(rounds), "--enable-pmu", "1" if pmu else "0",
            "--output", str(result), "--raw-output", str(raw), "--no-plot",
        ]
        self.run(name, argv, sudo=pmu, outputs=[result, raw])
        return result

    def phase_existence(self) -> None:
        phase: dict[str, Any] = {"state": "running", "access_profiles": {}}
        self.state["phases"]["existence"] = phase
        self.save()
        for access in ACCESS_TYPES:
            pmu_paths = [
                self.trigger_sweep(
                    f"exist-{access}-pmu-r{repeat}", access,
                    self.args.discovery_stride, self.args.discovery_stride,
                    1, self.args.max_discovery_accesses,
                    self.args.discovery_rounds, True,
                )
                for repeat in range(1, self.args.replicates + 1)
            ]
            cache_path = self.trigger_sweep(
                f"exist-{access}-cache", access,
                self.args.discovery_stride, self.args.discovery_stride,
                1, self.args.max_discovery_accesses,
                self.args.discovery_rounds, False,
            )
            if not self.args.execute:
                phase["access_profiles"][access] = {"state": "planned"}
                continue
            values = pmu_by_step(pmu_paths, self.args.discovery_stride)
            cache_rows = read_tsv(cache_path)
            pmu_first = first_replicated_pmu_activation(values, self.args.pmu_threshold)
            pmu_stable = first_stable_pmu_activation(values, self.args.pmu_threshold)
            cache_first = first_cache_activation(cache_rows, self.args.hit_threshold)
            supported = pmu_first is not None and cache_first is not None
            phase["access_profiles"][access] = {
                "state": "supported" if supported else "unsupported",
                "pmu_first_replicated_accesses": pmu_first,
                "pmu_first_all_replicates_accesses": pmu_stable,
                "cache_first_accesses": cache_first,
                "minimum_accesses": cache_first if supported else None,
                "robust_accesses_for_followup": (
                    max(cache_first or 0, pmu_stable or 0) + 2 if supported else None
                ),
                "pmu_by_access_count": {
                    str(step): {
                        "samples": samples, "median": median(samples),
                    } for step, samples in sorted(values.items())
                },
                "evidence": [str(path.relative_to(self.output)) for path in pmu_paths]
                + [str(cache_path.relative_to(self.output))],
            }
        phase["state"] = "completed" if self.args.execute else "planned"
        self.save()

    def active_profiles(self) -> dict[str, dict[str, Any]]:
        profiles = self.state.get("phases", {}).get("existence", {}).get(
            "access_profiles", {}
        )
        return {name: value for name, value in profiles.items()
                if value.get("state") == "supported"}

    def range_point(self, access: str, accesses: int, stride: int,
                    repeat: int = 1) -> float | None:
        path = self.trigger_sweep(
            f"range-{access}-s{stride}-r{repeat}", access, stride, stride,
            accesses, accesses, self.args.range_rounds, True,
        )
        if not self.args.execute:
            return None
        rows = read_tsv(path)
        return float(rows[0]["pmu_per_round"])

    def phase_range(self) -> None:
        phase: dict[str, Any] = {"state": "running", "access_profiles": {}}
        self.state["phases"]["range"] = phase
        self.save()
        for access, profile in self.active_profiles().items():
            accesses = int(profile["robust_accesses_for_followup"])
            points: dict[int, list[float]] = {}
            powers = []
            stride = 1
            while stride <= self.args.max_stride:
                powers.append(stride)
                stride *= 2
            for candidate in powers:
                value = self.range_point(access, accesses, candidate)
                if value is not None:
                    points[candidate] = [value]
            if not self.args.execute:
                phase["access_profiles"][access] = {"state": "planned"}
                continue
            positives = [key for key, values in points.items()
                         if median(values) is not None and median(values) >= self.args.pmu_threshold]
            negatives_after = [
                key for key, values in points.items()
                if positives and key > min(positives)
                and median(values) is not None and median(values) < self.args.pmu_threshold
            ]
            lower = max(positives) if positives else 0
            upper = min(negatives_after) if negatives_after else None
            if upper is not None:
                lo = max(key for key in positives if key < upper)
                hi = upper
                while hi - lo > 1:
                    middle = (lo + hi) // 2
                    value = self.range_point(access, accesses, middle)
                    points[middle] = [value or 0.0]
                    if value is not None and value >= self.args.pmu_threshold:
                        lo = middle
                    else:
                        hi = middle
                lower, upper = lo, hi
                for candidate in range(max(1, lower - 2), min(self.args.max_stride, upper + 2) + 1):
                    samples = points.setdefault(candidate, [])
                    start = len(samples) + 1
                    for repeat in range(start, self.args.replicates + 1):
                        value = self.range_point(access, accesses, candidate, repeat)
                        if value is not None:
                            samples.append(value)
            supported = [
                stride for stride, samples in points.items()
                if median(samples) is not None and median(samples) >= self.args.pmu_threshold
            ]
            phase["access_profiles"][access] = {
                "state": "observed" if supported else "inconclusive",
                "accesses": accesses,
                "maximum_supported_stride_lines": max(supported) if supported else None,
                "first_unsupported_stride_after_boundary": upper,
                "pmu_points": {
                    str(key): {"samples": values, "median": median(values)}
                    for key, values in sorted(points.items())
                },
                "limitations": [
                    "adaptive boundary search assumes a mostly monotonic supported interval",
                    "page-crossing behavior is not separated from stride magnitude in this phase",
                ],
            }
        phase["state"] = "completed" if self.args.execute else "planned"
        self.save()

    def phase_degree(self) -> None:
        phase: dict[str, Any] = {"state": "running", "access_profiles": {}}
        self.state["phases"]["degree"] = phase
        self.save()
        directory = self.output / "degree"
        directory.mkdir(exist_ok=True)
        for access, profile in self.active_profiles().items():
            train_step = int(profile["robust_accesses_for_followup"])
            max_step = min(train_step + self.args.degree_extra_steps,
                           self.args.degree_probe_positions)
            result = directory / f"{access}.tsv"
            raw = directory / f"{access}.raw.txt"
            argv = [
                sys.executable, str(DEGREE), "--arch", self.args.arch,
                "--core", str(self.args.core), "--access", access,
                "--stride", str(self.args.discovery_stride),
                "--max-step", str(max_step),
                "--probe-positions", str(self.args.degree_probe_positions),
                "--rounds", str(self.args.degree_rounds),
                "--hit-threshold-ns", str(self.args.hit_threshold),
                "--output", str(result), "--raw-output", str(raw), "--no-plot",
            ]
            if not (self.args.resume and result.is_file()):
                self.run(f"degree-{access}", argv, outputs=[result, raw])
            phase["access_profiles"][access] = (
                parse_degree(result, train_step, self.args.hit_threshold)
                if self.args.execute else {"state": "planned"}
            )
        phase["state"] = "completed" if self.args.execute else "planned"
        self.save()

    def phase_index(self) -> None:
        phase: dict[str, Any] = {"state": "running", "access_profiles": {}}
        self.state["phases"]["index"] = phase
        self.save()
        directory = self.output / "index"
        directory.mkdir(exist_ok=True)
        for access, profile in self.active_profiles().items():
            accesses = int(profile["minimum_accesses"])
            mode_run = self.run(
                f"index-mode-{access}",
                [
                    sys.executable, str(INDEX_MODE), "--arch", self.args.arch,
                    "--core", str(self.args.core), "--access", access,
                    "--stride", str(self.args.discovery_stride),
                    "--train-step", str(accesses), "--rounds", str(self.args.index_rounds),
                    "--repeat", "1", "--hit-threshold-ns", str(self.args.hit_threshold),
                ],
                sudo=True,
            )
            if not self.args.execute or mode_run is None:
                phase["access_profiles"][access] = {"state": "planned"}
                continue
            mode = parse_index_mode(mode_run.stdout)
            result: dict[str, Any] = {"mode": mode}
            if not mode.get("sanity_passed"):
                result["state"] = "inconclusive"
                result["blocked"] = ["bit_scans", "capacity"]
                phase["access_profiles"][access] = result
                continue
            if mode.get("pc_equality_required_candidate"):
                pc_path = directory / f"{access}-pc-bits.tsv"
                raw_path = directory / f"{access}-pc-bits.raw.txt"
                self.run(
                    f"index-pc-{access}",
                    [
                        sys.executable, str(INDEX_PC), "--arch", self.args.arch,
                        "--core", str(self.args.core), "--access", access,
                        "--stride", str(self.args.discovery_stride),
                        "--accesses", str(accesses), "--min-diff-bit", "3",
                        "--max-diff-bit", "47", "--rounds", str(self.args.index_rounds),
                        "--enable-pmu", "1", "--enable-probe", "0",
                        "--output", str(pc_path), "--raw-output", str(raw_path), "--no-plot",
                    ],
                    sudo=True, outputs=[pc_path, raw_path],
                )
                result["pc_bits"] = parse_pc_bits(pc_path)
            else:
                result["pc_bits"] = {"state": "skipped", "reason": "T1 stayed positive"}
            result["va_bits"] = {
                "state": "candidate" if mode.get("va_equality_required_candidate") else "skipped",
                "experiment": str(INDEX_VA_XOR.relative_to(REPO)),
                "reason": "pairwise scan is deferred unless T2 requires VA equality",
            }
            result["pa_bits"] = {
                "state": "candidate" if mode.get("pa_sufficient_candidate") else "unresolved",
                "experiment": str(INDEX_PA_XOR.relative_to(REPO)),
                "reason": "PA pairwise scan requires privileged pagemap allocation and a validated candidate basis",
            }
            result["state"] = "observed"
            phase["access_profiles"][access] = result
        phase["state"] = "completed" if self.args.execute else "planned"
        self.save()

    def phase_capacity(self) -> None:
        phase: dict[str, Any] = {"state": "running", "access_profiles": {}}
        self.state["phases"]["capacity"] = phase
        self.save()
        directory = self.output / "capacity"
        directory.mkdir(exist_ok=True)
        index_profiles = self.state.get("phases", {}).get("index", {}).get(
            "access_profiles", {}
        )
        for access, profile in self.active_profiles().items():
            index = index_profiles.get(access, {})
            if index.get("mode", {}).get("sanity_passed") is not True:
                phase["access_profiles"][access] = {
                    "state": "blocked", "reason": "index-mode sanity did not pass",
                }
                continue
            accesses = int(profile["minimum_accesses"])
            run = self.run(
                f"capacity-{access}",
                [
                    sys.executable, str(ENTRY_NUMS), "--arch", self.args.arch,
                    "--core", str(self.args.core), "--access", access,
                    "--stride", str(self.args.discovery_stride),
                    "--train-step", str(accesses), "--other-pages-access", "same",
                    "--other-pages-sweep", "--other-pages-min", "0",
                    "--other-pages-max", str(self.args.max_competitors),
                    "--rounds", str(self.args.capacity_rounds), "--no-plot", "--no-dump",
                ],
                sudo=True,
            )
            if not self.args.execute or run is None:
                phase["access_profiles"][access] = {"state": "planned"}
                continue
            observations = []
            pattern = re.compile(
                r"other_pages=(\d+).*predicted_position=\d+.*avg_\w+=(\d+).*prefetched=(yes|no)"
            )
            for line in clean_ansi(run.stdout).splitlines():
                match = pattern.search(line)
                if match:
                    observations.append({
                        "competitors": int(match.group(1)),
                        "latency": int(match.group(2)),
                        "prefetched": match.group(3) == "yes",
                    })
            first_failure = next(
                (row["competitors"] for row in observations if not row["prefetched"]),
                None,
            )
            result: dict[str, Any] = {
                "state": "observed" if observations else "inconclusive",
                "pressure_curve": observations,
                "first_observed_failure": first_failure,
                "limitations": [
                    "entry pressure is not an exact capacity unless all competitors are validated same-set different-tag colliders",
                ],
            }
            if first_failure and first_failure >= 2:
                replacement_tsv = directory / f"{access}-replacement.tsv"
                replacement_summary = directory / f"{access}-replacement-summary.tsv"
                replacement_raw = directory / f"{access}-replacement.raw.txt"
                self.run(
                    f"replacement-{access}",
                    [
                        sys.executable, str(REPLACEMENT), "--arch", self.args.arch,
                        "--core", str(self.args.core), "--access", access,
                        "--stride", str(self.args.discovery_stride),
                        "--accesses", str(accesses), "--entries", str(first_failure),
                        "--rounds", str(self.args.replacement_rounds),
                        "--repeat-runs", str(self.args.replicates),
                        "--output", str(replacement_tsv),
                        "--summary-output", str(replacement_summary),
                        "--raw-output", str(replacement_raw),
                    ],
                    outputs=[replacement_tsv, replacement_summary],
                )
                if replacement_summary.is_file():
                    result["replacement_summary"] = read_tsv(replacement_summary)
            phase["access_profiles"][access] = result
        phase["state"] = "completed" if self.args.execute else "planned"
        self.save()

    def phase_context(self) -> None:
        phase: dict[str, Any] = {"state": "running", "access_profiles": {}}
        self.state["phases"]["context"] = phase
        self.save()
        for access, profile in self.active_profiles().items():
            run = self.run(
                f"context-{access}",
                [
                    sys.executable, str(CROSS_CONTEXT), "--arch", self.args.arch,
                    "--core", str(self.args.core), "--access", access,
                    "--stride", str(self.args.discovery_stride),
                    "--train-accesses", str(profile["minimum_accesses"]),
                    "--rounds", str(self.args.context_rounds),
                    "--probe-positions", str(self.args.context_probe_positions),
                    "--hit-threshold-ns", str(self.args.hit_threshold),
                    "--threshold-ns", str(self.args.hit_threshold), "--skip-trustzone",
                ],
            )
            if not self.args.execute or run is None:
                phase["access_profiles"][access] = {"state": "planned"}
                continue
            observations = {}
            pattern = re.compile(r"^(cross-[a-z0-9-]+):\s+(yes|no|error)(?:\s+avg_ns=(\d+))?")
            for line in clean_ansi(run.stdout).splitlines():
                match = pattern.match(line.strip())
                if match:
                    observations[match.group(1)] = {
                        "supported": match.group(2) == "yes",
                        "status": match.group(2),
                        "avg_ns": int(match.group(3)) if match.group(3) else None,
                    }
            phase["access_profiles"][access] = {
                "state": "observed" if observations else "inconclusive",
                "contexts": observations,
                "limitations": [
                    "a negative result may mean state is private, cleared, or the trigger key changed",
                    "TrustZone is skipped unless OP-TEE availability is explicitly established",
                ],
            }
        phase["state"] = "completed" if self.args.execute else "planned"
        self.save()

    def execute(self) -> None:
        handlers = {
            "existence": self.phase_existence,
            "range": self.phase_range,
            "degree": self.phase_degree,
            "index": self.phase_index,
            "capacity": self.phase_capacity,
            "context": self.phase_context,
        }
        selected = PHASES if self.args.phases == ["all"] else self.args.phases
        if selected != ("existence",) and "existence" not in self.state.get("phases", {}):
            raise CampaignError("existence phase must be run before dependent phases")
        for phase in selected:
            if phase != "existence" and not self.active_profiles():
                self.state["phases"][phase] = {
                    "state": "blocked", "reason": "no supported Load/Store profile",
                }
                self.save()
                continue
            print(f"\n## phase: {phase}", flush=True)
            handlers[phase]()
        self.state["status"] = "completed" if self.args.execute else "planned"
        self.save()


def parse_args(argv: list[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Run a resumable Load/Store stride-prefetcher reverse-engineering campaign"
    )
    parser.add_argument("--arch", default="X925", choices=["X925"])
    parser.add_argument("--core", type=int, default=6)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--execute", action="store_true")
    parser.add_argument("--resume", action="store_true")
    parser.add_argument("--phases", nargs="+", choices=["all", *PHASES], default=["all"])
    parser.add_argument("--discovery-stride", type=int, default=3)
    parser.add_argument("--max-discovery-accesses", type=int, default=24)
    parser.add_argument("--replicates", type=int, default=3)
    parser.add_argument("--pmu-threshold", type=float, default=PMU_THRESHOLD)
    parser.add_argument("--hit-threshold", type=int, default=80)
    parser.add_argument("--discovery-rounds", type=int, default=200)
    parser.add_argument("--range-rounds", type=int, default=100)
    parser.add_argument("--max-stride", type=int, default=4096)
    parser.add_argument("--degree-rounds", type=int, default=100)
    parser.add_argument("--degree-probe-positions", type=int, default=40)
    parser.add_argument("--degree-extra-steps", type=int, default=6)
    parser.add_argument("--index-rounds", type=int, default=1000)
    parser.add_argument("--capacity-rounds", type=int, default=200)
    parser.add_argument("--max-competitors", type=int, default=64)
    parser.add_argument("--replacement-rounds", type=int, default=500)
    parser.add_argument("--context-rounds", type=int, default=1000)
    parser.add_argument("--context-probe-positions", type=int, default=64)
    args = parser.parse_args(argv)
    if args.core < 0 or args.discovery_stride < 1 or args.replicates < 1:
        parser.error("core must be non-negative; stride and replicates must be positive")
    if args.max_discovery_accesses < 2 or args.max_stride < 1:
        parser.error("discovery access and stride bounds are invalid")
    return args


def main(argv: list[str] | None = None) -> int:
    args = parse_args(argv)
    try:
        StrideCampaign(args).execute()
    except CampaignError as exc:
        print(f"stride campaign error: {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
