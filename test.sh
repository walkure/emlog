#!/bin/bash
#
# Kept for backward compatibility (this used to be the only smoke test,
# with a hardcoded major number that broke as soon as a different major
# got assigned). The actual test now lives in test/, alongside the rest
# of the regression suite added for the Finding 1-7 fixes -- see
# test/PROCEDURE.md for details and how to run the others.
exec "$(dirname "$0")/test/run_basic_smoke_test.sh" "$@"
