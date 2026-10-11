from __future__ import annotations

import argparse
import json
import sys
from datetime import datetime, timezone
from pathlib import Path

from .protocol import (PROTOCOL_ROOT, ProtocolError, load_experiments,
                       load_targets, resolve_variables)
from .runner import make_plan, run_plan


def parser() -> argparse.ArgumentParser:
    root = argparse.ArgumentParser(description="Unified prefetcher experiment protocol")
    commands = root.add_subparsers(dest="command", required=True)
    commands.add_parser("validate", help="Validate all protocol documents")
    commands.add_parser("list-targets", help="List registered targets")
    commands.add_parser("list-experiments", help="List registered experiments")
    plan = commands.add_parser("plan", help="Render a deterministic experiment plan")
    plan.add_argument("experiment")
    plan.add_argument("--target", required=True)
    plan.add_argument("--set", action="append", default=[], metavar="NAME=VALUE")
    plan.add_argument("--output-dir", type=Path)
    plan.add_argument("--execute", action="store_true",
                      help="Execute the rendered plan; omitted by default for safety")
    return root


def default_output_dir(experiment: str, target: str) -> Path:
    stamp = datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%SZ")
    return PROTOCOL_ROOT.parent / "prefetch-agent-results" / f"{stamp}-{target}-{experiment}"


def main(argv: list[str] | None = None) -> int:
    args = parser().parse_args(argv)
    try:
        targets = load_targets()
        experiments = load_experiments()
        if args.command == "validate":
            print(f"validated {len(targets)} targets and {len(experiments)} experiments")
            return 0
        if args.command == "list-targets":
            for target in targets.values():
                print(f"{target['id']}\t{target['label']}\tCPU{target['cpu']['core']}")
            return 0
        if args.command == "list-experiments":
            for experiment in experiments.values():
                print(f"{experiment['id']}\t{experiment['family']}\t{experiment['goal']}")
            return 0
        if args.target not in targets:
            raise ProtocolError(f"unknown target {args.target!r}")
        if args.experiment not in experiments:
            raise ProtocolError(f"unknown experiment {args.experiment!r}")
        target = targets[args.target]
        experiment = experiments[args.experiment]
        variables = resolve_variables(experiment, args.set)
        output_dir = args.output_dir or default_output_dir(args.experiment, args.target)
        plan = make_plan(experiment, target, variables, output_dir)
        print(json.dumps(plan, indent=2))
        if plan["missing_capabilities"]:
            raise ProtocolError("target lacks capabilities: " + ", ".join(plan["missing_capabilities"]))
        if not args.execute:
            return 0
        result, return_code = run_plan(plan, experiment, target)
        print(json.dumps({"status": result["status"], "result": str(output_dir / "result.json")}, indent=2))
        return return_code
    except ProtocolError as exc:
        print(f"protocol error: {exc}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
