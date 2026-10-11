from __future__ import annotations

import json
import re
from pathlib import Path
from typing import Any


REPO_ROOT = Path(__file__).resolve().parent.parent
PROTOCOL_ROOT = REPO_ROOT / "protocol"
TOKEN_RE = re.compile(r"\{([a-zA-Z0-9_.-]+)\}")


class ProtocolError(ValueError):
    pass


def load_json(path: Path) -> dict[str, Any]:
    try:
        value = json.loads(path.read_text())
    except OSError as exc:
        raise ProtocolError(f"cannot read {path}: {exc}") from exc
    except json.JSONDecodeError as exc:
        raise ProtocolError(f"invalid JSON in {path}: {exc}") from exc
    if not isinstance(value, dict):
        raise ProtocolError(f"{path} must contain a JSON object")
    return value


def _require(document: dict[str, Any], fields: tuple[str, ...], source: Path) -> None:
    missing = [field for field in fields if field not in document]
    if missing:
        raise ProtocolError(f"{source}: missing fields: {', '.join(missing)}")


def _check_id(value: Any, source: Path) -> None:
    if not isinstance(value, str) or not re.fullmatch(r"[a-z0-9][a-z0-9_.-]*", value):
        raise ProtocolError(f"{source}: invalid id {value!r}")


def validate_target(target: dict[str, Any], source: Path) -> None:
    _require(target, ("schema_version", "id", "label", "architecture", "cpu",
                      "timing", "capabilities", "execution"), source)
    _check_id(target["id"], source)
    if target["schema_version"] != "1.0":
        raise ProtocolError(f"{source}: unsupported schema_version")
    cpu = target["cpu"]
    timing = target["timing"]
    if not isinstance(cpu, dict) or not isinstance(cpu.get("core"), int):
        raise ProtocolError(f"{source}: cpu.core must be an integer")
    if cpu["core"] < 0:
        raise ProtocolError(f"{source}: cpu.core must be non-negative")
    if not isinstance(timing, dict) or timing.get("cache_hit_threshold") is None:
        raise ProtocolError(f"{source}: timing.cache_hit_threshold is required")
    if not isinstance(target["capabilities"], list):
        raise ProtocolError(f"{source}: capabilities must be an array")


def validate_experiment(experiment: dict[str, Any], source: Path) -> None:
    _require(experiment, ("schema_version", "id", "family", "goal", "requires",
                          "variables", "command", "observations", "controls",
                          "artifacts", "sources"), source)
    _check_id(experiment["id"], source)
    if experiment["schema_version"] != "1.0":
        raise ProtocolError(f"{source}: unsupported schema_version")
    command = experiment["command"]
    if not isinstance(command, dict) or not isinstance(command.get("argv"), list):
        raise ProtocolError(f"{source}: command.argv must be an array")
    if not command["argv"] or not all(isinstance(item, str) for item in command["argv"]):
        raise ProtocolError(f"{source}: command.argv must contain strings")
    variables = experiment["variables"]
    if not isinstance(variables, dict):
        raise ProtocolError(f"{source}: variables must be an object")
    for name, spec in variables.items():
        if not isinstance(spec, dict) or "type" not in spec or "default" not in spec:
            raise ProtocolError(f"{source}: variable {name} needs type and default")


def load_targets() -> dict[str, dict[str, Any]]:
    targets: dict[str, dict[str, Any]] = {}
    for path in sorted((PROTOCOL_ROOT / "targets").glob("*.json")):
        target = load_json(path)
        validate_target(target, path)
        if target["id"] in targets:
            raise ProtocolError(f"duplicate target id: {target['id']}")
        targets[target["id"]] = target
    return targets


def load_experiments() -> dict[str, dict[str, Any]]:
    experiments: dict[str, dict[str, Any]] = {}
    for path in sorted((PROTOCOL_ROOT / "experiments").glob("*.json")):
        experiment = load_json(path)
        validate_experiment(experiment, path)
        if experiment["id"] in experiments:
            raise ProtocolError(f"duplicate experiment id: {experiment['id']}")
        experiments[experiment["id"]] = experiment
    return experiments


def _convert(name: str, value: str, spec: dict[str, Any]) -> Any:
    kind = spec["type"]
    try:
        if kind == "integer":
            converted: Any = int(value, 0)
        elif kind == "number":
            converted = float(value)
        elif kind == "boolean":
            lowered = value.lower()
            if lowered not in {"true", "false"}:
                raise ValueError("expected true or false")
            converted = lowered == "true"
        elif kind == "string":
            converted = value
        else:
            raise ProtocolError(f"variable {name}: unsupported type {kind}")
    except ValueError as exc:
        raise ProtocolError(f"variable {name}: cannot parse {value!r} as {kind}") from exc
    if "minimum" in spec and converted < spec["minimum"]:
        raise ProtocolError(f"variable {name}: must be >= {spec['minimum']}")
    if "maximum" in spec and converted > spec["maximum"]:
        raise ProtocolError(f"variable {name}: must be <= {spec['maximum']}")
    if "enum" in spec and converted not in spec["enum"]:
        raise ProtocolError(f"variable {name}: must be one of {spec['enum']}")
    return converted


def resolve_variables(experiment: dict[str, Any], overrides: list[str]) -> dict[str, Any]:
    variables = {name: spec["default"] for name, spec in experiment["variables"].items()}
    for override in overrides:
        if "=" not in override:
            raise ProtocolError(f"invalid --set {override!r}; expected NAME=VALUE")
        name, value = override.split("=", 1)
        if name not in experiment["variables"]:
            raise ProtocolError(f"unknown variable {name!r} for {experiment['id']}")
        variables[name] = _convert(name, value, experiment["variables"][name])
    for name, spec in experiment["variables"].items():
        variables[name] = _convert(name, str(variables[name]), spec)
    return variables


def flatten(prefix: str, value: Any, output: dict[str, str]) -> None:
    if isinstance(value, dict):
        for key, child in value.items():
            flatten(f"{prefix}.{key}" if prefix else key, child, output)
    elif isinstance(value, list):
        output[prefix] = ",".join(str(item) for item in value)
    elif isinstance(value, bool):
        output[prefix] = "true" if value else "false"
    else:
        output[prefix] = str(value)


def render_argv(experiment: dict[str, Any], target: dict[str, Any],
                variables: dict[str, Any], output_dir: Path) -> list[str]:
    context: dict[str, str] = {}
    flatten("target", target, context)
    flatten("", variables, context)
    context["run.output_dir"] = str(output_dir.resolve())
    context["repo"] = str(REPO_ROOT)

    def replace(match: re.Match[str]) -> str:
        token = match.group(1)
        if token not in context:
            raise ProtocolError(f"{experiment['id']}: unknown template token {token}")
        return context[token]

    return [TOKEN_RE.sub(replace, item) for item in experiment["command"]["argv"]]


def check_compatibility(experiment: dict[str, Any], target: dict[str, Any]) -> list[str]:
    available = set(target["capabilities"])
    return sorted(set(experiment["requires"]) - available)
