# Prefetch Agent experiment protocol

This protocol establishes a common, machine-readable boundary around the
existing microbenchmarks. It does not replace their C/Python implementations.

## Protocol objects

- `protocol/targets/*.json` records CPU selection, timing calibration,
  capabilities, PMU events, and execution policy in one place.
- `protocol/experiments/*.json` records an experiment's goal, variables,
  controls, observations, source files, artifacts, and deterministic argv.
- `protocol/schemas/*.schema.json` is the versioned interchange contract.
- Every executed run receives a `result.json` envelope containing provenance,
  source hashes, command, host metadata, artifacts, and evidence state.

The runner uses only the Python standard library. JSON Schema files are
provided for external tooling; the CLI also performs essential validation
without requiring the `jsonschema` package.

## Safety model

Planning is the default. A command is run only when `--execute` is present.
Commands are represented as argv arrays and never passed through a shell.
Experiments that manage the direct-PMU kernel module declare
`requires_privilege: true`. Their existing wrappers retain ownership of module
cleanup and refusal to replace an externally loaded module.

## Usage

Run these commands from the repository root:

```sh
python3 -m prefetch_agent validate
python3 -m prefetch_agent list-targets
python3 -m prefetch_agent list-experiments
python3 -m prefetch_agent plan region.existence --target cortex-x925-cpu6
python3 -m prefetch_agent plan cmc.existence \
  --target cortex-a725-cpu4 --set rounds=1000 --set training_replays=4
```

To run a plan on the matching target host:

```sh
python3 -m prefetch_agent plan calibration.cache-latency \
  --target cortex-x925-cpu6 --execute
```

Results default to `prefetch-agent-results/<UTC>-<target>-<experiment>/`.
Use `--output-dir` to select another run directory.

## Adding an experiment

1. Add one JSON file under `protocol/experiments/`.
2. Use an argv array with `{repo}`, `{run.output_dir}`, `{target.*}`, and
   declared variable placeholders.
3. Name the true control cases and expected observations explicitly.
4. List every source file that affects the binary so it is hashed per run.
5. Run protocol validation and unit tests.

The initial adapters cover timing calibration, Region existence, CMC
existence, and BOP existence. Observation parsing and automatic evidence
promotion deliberately remain separate: raw execution alone must not turn a
PMU delta into a hardware-structure claim.
