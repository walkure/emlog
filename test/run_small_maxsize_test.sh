#!/bin/bash
#
# Regression test for Finding 3: /dev/emlog used to be created with a
# hardcoded minor number (256), which falls outside the registered chrdev
# minor range whenever the module is loaded with emlog_max_size < 256 (a
# documented, supported option) -- making /dev/emlog permanently -ENXIO.
#
# ** Loads and unloads the emlog kernel module (sudo insmod / sudo rmmod),
#    specifically with a small emlog_max_size. **
#
# Usage: test/run_small_maxsize_test.sh

set -euo pipefail
cd "$(dirname "$0")/.."

fail=0

cleanup() {
    if lsmod | grep -q '^emlog '; then
        sudo rmmod emlog 2>/dev/null || true
    fi
}
trap cleanup EXIT

echo "== building =="
make >/dev/null

if lsmod | grep -q '^emlog '; then
    echo "!! emlog module already loaded -- unload it first (sudo rmmod emlog) so this test starts clean !!"
    exit 1
fi

echo "== loading module with emlog_max_size=64 (below the old hardcoded minor 256) =="
sudo insmod ./emlog.ko emlog_max_size=64

if [ ! -c /dev/emlog ]; then
    echo "!! /dev/emlog was not created at all -- can't test !!"
    exit 1
fi

echo "== attempting to open /dev/emlog =="
# device_create() leaves it root:root 0600 by default, so this needs sudo.
if sudo ./emlog_stat /dev/emlog; then
    echo "  OK: /dev/emlog opened and answered EMLOG_GET_STATUS with emlog_max_size=64"
else
    echo "  FAIL: /dev/emlog exists but could not be opened/queried (this is the Finding 3 bug if it reproduces)"
    fail=1
fi

sudo rmmod emlog

if [ "$fail" -ne 0 ]; then
    echo "== SMALL-MAXSIZE TEST FAILED =="
    exit 1
fi
echo "== SMALL-MAXSIZE TEST PASSED =="
