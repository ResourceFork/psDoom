#!/usr/bin/env python3
"""Run the harness tests with an explicit scratch root.

    python3 -I harness/tests/run_tests.py --scratch /abs/build/harness-tests

Every test makes its own directory under --scratch and points the harness's
state_dir there; nothing is read from or written to the home directory, and
no environment variable chooses a location. ctest runs this (CMakeLists.txt).
"""

import argparse
import os
import sys
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)   # -I drops the script dir from sys.path

import test_harness  # noqa: E402


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--scratch", required=True)
    a = p.parse_args()
    scratch = os.path.normpath(a.scratch)
    if not os.path.isabs(scratch) or scratch == os.sep:
        sys.exit("--scratch must be an absolute path other than /")
    os.makedirs(scratch, exist_ok=True)
    test_harness.SCRATCH = scratch
    os.chdir(scratch)   # hostile-label checks look for stray files here too
    suite = unittest.defaultTestLoader.loadTestsFromModule(test_harness)
    res = unittest.TextTestRunner(verbosity=2).run(suite)
    return 0 if res.wasSuccessful() else 1


if __name__ == "__main__":
    sys.exit(main())
