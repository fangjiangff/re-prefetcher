"""Run test0-trigger with corrected A725 event and absolute local headers."""

from __future__ import annotations

import importlib.util
import tempfile
from pathlib import Path


REPO = Path(__file__).resolve().parent.parent
WRAPPER = REPO / "re0-stride-prefetcher" / "test0-trigger" / "test0-trigger.py"
SOURCE = REPO / "re0-stride-prefetcher" / "test0-trigger" / "test0-trigger.c"


def main() -> int:
    spec = importlib.util.spec_from_file_location("a725_test0_trigger_v2", WRAPPER)
    if spec is None or spec.loader is None:
        raise RuntimeError(f"cannot load {WRAPPER}")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)

    source = SOURCE.read_text()
    substitutions = {
        '#define SWEEP_PMU_EVENT_NAME "l2d_cache_prf"':
            '#define SWEEP_PMU_EVENT_NAME "l1d_cache_hwprf"',
        '#include "../pmu.h"':
            f'#include "{REPO / "re0-stride-prefetcher" / "pmu.h"}"',
        '#include "../../until.h"': f'#include "{REPO / "until.h"}"',
    }
    for old, new in substitutions.items():
        if old not in source:
            raise RuntimeError(f"A725 adapter marker is missing: {old}")
        source = source.replace(old, new, 1)

    with tempfile.TemporaryDirectory(prefix="a725-trigger-") as directory:
        adapted = Path(directory) / "test0-trigger-a725.c"
        adapted.write_text(source)
        module.SRC = str(adapted)
        return module.main()


if __name__ == "__main__":
    raise SystemExit(main())
