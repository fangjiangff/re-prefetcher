"""Run test0-trigger with the Cortex-A725 L1 hardware-prefetch PMU event."""

from __future__ import annotations

import importlib.util
import sys
import tempfile
from pathlib import Path


HERE = Path(__file__).resolve().parent.parent
WRAPPER = HERE / "re0-stride-prefetcher" / "test0-trigger" / "test0-trigger.py"
SOURCE = HERE / "re0-stride-prefetcher" / "test0-trigger" / "test0-trigger.c"
PMU_HEADER = HERE / "re0-stride-prefetcher" / "pmu.h"


def main() -> int:
    spec = importlib.util.spec_from_file_location("a725_test0_trigger", WRAPPER)
    if spec is None or spec.loader is None:
        raise RuntimeError(f"cannot load {WRAPPER}")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)

    source = SOURCE.read_text()
    old = '#define SWEEP_PMU_EVENT_NAME "l2d_cache_prf"'
    new = '#define SWEEP_PMU_EVENT_NAME "l1d_cache_hwprf"'
    if old not in source:
        raise RuntimeError("A725 trigger event selection marker is missing")
    source = source.replace(old, new, 1)
    source = source.replace('#include "../pmu.h"', f'#include "{PMU_HEADER}"', 1)

    with tempfile.TemporaryDirectory(prefix="a725-trigger-") as directory:
        adapted = Path(directory) / "test0-trigger-a725.c"
        adapted.write_text(source)
        module.SRC = str(adapted)
        return module.main()


if __name__ == "__main__":
    raise SystemExit(main())
