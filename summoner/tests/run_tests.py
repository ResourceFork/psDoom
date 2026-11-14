#!/usr/bin/env python3
"""Run the Summoner tests. Needs only Python 3.10+, no build system:

    python3 -I summoner/tests/run_tests.py [--scratch /abs/dir]

Every test makes its own directory under the scratch root and points the
Summoner's state_dir there. The scratch root is --scratch when given (ctest
passes one in its build tree), otherwise `.scratch/` next to this file
(git-ignored by summoner/.gitignore). A symlinked root is refused. It is
never the home directory and never chosen by an environment variable. The tests use only files inside summoner/, so this
folder runs the same on its own.
"""

import argparse
import os
import sys
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)   # -I drops the script dir from sys.path

import test_summoner  # noqa: E402


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--scratch", default=os.path.join(HERE, ".scratch"))
    a = p.parse_args()
    scratch = os.path.normpath(a.scratch)
    if not os.path.isabs(scratch) or scratch == os.sep:
        sys.exit("--scratch must be an absolute path other than /")
    if os.path.islink(scratch):
        sys.exit("scratch root must not be a symlink: %s" % scratch)
    os.makedirs(scratch, exist_ok=True)
    if os.path.islink(scratch) or not os.path.isdir(scratch):
        sys.exit("scratch root is not a plain directory: %s" % scratch)
    test_summoner.SCRATCH = scratch
    os.chdir(scratch)   # hostile-label checks look for stray files here too
    suite = unittest.defaultTestLoader.loadTestsFromModule(test_summoner)
    res = unittest.TextTestRunner(verbosity=2).run(suite)
    os.chdir(HERE)
    try:
        os.rmdir(scratch)   # only if every test cleaned up after itself
    except OSError:
        pass
    return 0 if res.wasSuccessful() else 1


if __name__ == "__main__":
    sys.exit(main())
