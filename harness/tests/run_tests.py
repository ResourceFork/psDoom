#!/usr/bin/env python3
"""Run the harness tests. Needs only Python 3.10+, no build system:

    python3 -I harness/tests/run_tests.py [--scratch /abs/dir]

Every test makes its own directory under the scratch root and points the
harness's state_dir there. The scratch root is --scratch when given (ctest
passes one in its build tree), otherwise `.scratch/` next to this file
(git-ignored). It is never the home directory and never chosen by an
environment variable. The tests use only files inside harness/, so this
folder runs the same on its own.
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
    p.add_argument("--scratch", default=os.path.join(HERE, ".scratch"))
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
