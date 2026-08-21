#!/bin/bash
#
# Runs all regression tests in order. Each sub-test loads/unloads the
# emlog kernel module itself and cleans up after itself, so they can run
# back-to-back. Stops at the first failure (its own output explains what
# broke); rerun that one script alone to iterate.
#
# See test/PROCEDURE.md for what each test checks and how to interpret
# a failure, and for how to run any of them individually.
#
# Usage: test/run_all.sh

set -euo pipefail
cd "$(dirname "$0")/.."

echo "############################################"
echo "# 1/4 basic smoke test"
echo "############################################"
test/run_basic_smoke_test.sh

echo
echo "############################################"
echo "# 2/4 small emlog_max_size / Finding 3"
echo "############################################"
test/run_small_maxsize_test.sh

echo
echo "############################################"
echo "# 3/4 kernel open/close race stress / Finding 1"
echo "############################################"
test/run_kernel_race_test.sh

echo
echo "############################################"
echo "# 4/4 emlog_fuse tests / Findings 2, 5, 6"
echo "############################################"
echo "loading module for the fuse tests..."
sudo insmod ./emlog.ko
test/run_fuse_tests.sh
sudo rmmod emlog

echo
echo "############################################"
echo "# ALL TESTS PASSED"
echo "############################################"
