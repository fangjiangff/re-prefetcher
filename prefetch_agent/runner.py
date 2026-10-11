from __future__ import annotations

import hashlib
import json
import os
import platform
import shlex
import subprocess
from datetime import datetime, timezone
from pathlib import Path
from typing import Any

from .protocol import REPO_ROOT, check_compatibility, render_argv


def utc_now() -> str:
    return datetime.now(timezone.utc).isoformat()


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for block in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def source_fingerprints(experiment: dict[str, Any]) -> list[dict[str, Any]]:
    records = []
    for relative in experiment["sources"]:
        path = (REPO_ROOT / relative).resolve()
        record: dict[str, Any] = {"path": relative, "exists": path.is_file()}
        if path.is_file():
            record.update({"size": path.stat().st_size, "sha256": sha256(path)})
        records.append(record)
    return records


def host_metadata() -> dict[str, Any]:
    return {
        "hostname": platform.node(),
        "machine": platform.machine(),
        "kernel": platform.release(),
        "python": platform.python_version(),
        "pid": os.getpid(),
    }


def make_plan(experiment: dict[str, Any], target: dict[str, Any],
              variables: dict[str, Any], output_dir: Path) -> dict[str, Any]:
    missing = check_compatibility(experiment, target)
    argv = render_argv(experiment, target, variables, output_dir)
    return {
        "schema_version": "1.0",
        "experiment_id": experiment["id"],
        "target_id": target["id"],
        "variables": variables,
        "command": {"argv": argv, "display": shlex.join(argv), "cwd": str(REPO_ROOT)},
        "missing_capabilities": missing,
        "requires_privilege": experiment["command"].get("requires_privilege", False),
        "output_dir": str(output_dir.resolve()),
    }


def run_plan(plan: dict[str, Any], experiment: dict[str, Any],
             target: dict[str, Any]) -> tuple[dict[str, Any], int]:
    output_dir = Path(plan["output_dir"])
    output_dir.mkdir(parents=True, exist_ok=True)
    started = utc_now()
    completed = subprocess.run(
        plan["command"]["argv"], cwd=REPO_ROOT, text=True,
        stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=False,
    )
    finished = utc_now()
    (output_dir / "stdout.txt").write_text(completed.stdout)
    (output_dir / "stderr.txt").write_text(completed.stderr)

    artifacts = []
    for path in sorted(output_dir.rglob("*")):
        if path.is_file() and path.name != "result.json":
            artifacts.append({
                "path": str(path.relative_to(output_dir)),
                "media_type": "text/plain" if path.suffix in {".txt", ".tsv", ".csv"} else "application/octet-stream",
                "size": path.stat().st_size,
                "sha256": sha256(path),
            })

    status = "completed" if completed.returncode == 0 else "failed"
    result = {
        "schema_version": "1.0",
        "run_id": output_dir.name,
        "status": status,
        "experiment": {
            "id": experiment["id"],
            "family": experiment["family"],
            "variables": plan["variables"],
            "sources": source_fingerprints(experiment),
        },
        "target": {"id": target["id"], "label": target["label"]},
        "execution": {
            "started_at": started,
            "finished_at": finished,
            "return_code": completed.returncode,
            "command": plan["command"],
            "host": host_metadata(),
        },
        "observations": [],
        "evidence": {
            "state": "unassessed",
            "reason": "Protocol runner captured raw artifacts; an experiment adapter must promote observations.",
        },
        "artifacts": artifacts,
        "warnings": [],
    }
    (output_dir / "result.json").write_text(json.dumps(result, indent=2) + "\n")
    return result, completed.returncode
