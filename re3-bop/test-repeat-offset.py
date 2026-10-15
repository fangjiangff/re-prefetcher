#!/usr/bin/env python3

import importlib.util
from pathlib import Path


HERE = Path(__file__).resolve().parent
spec = importlib.util.spec_from_file_location("test_bop_runner", HERE / "test-bop.py")
driver = importlib.util.module_from_spec(spec)
spec.loader.exec_module(driver)

driver.SRC = HERE / "test-repeat-offset.c"
driver.BINARY_STEM = "test-repeat-offset"
driver.RESULT_STEM = "repeat-offset"
driver.DISPLAY_NAME = "BOP repeated-function offset test"
driver.TRAINING_ACCESS_TYPES = ("load",)
driver.RUNNER_DESCRIPTION = (
    "Build and run the BOP test that repeatedly calls one function whose "
    "fixed load PCs access A_i and A_i+d."
)


if __name__ == "__main__":
    raise SystemExit(driver.main())
