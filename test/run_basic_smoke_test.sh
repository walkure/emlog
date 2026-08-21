#!/bin/bash
#
# Basic functional smoke test: build, load, mknod, write/read via nbcat,
# check emlog_stat, unload. Same idea as the top-level test.sh, but uses
# the actual (dynamically assigned) major number instead of a hardcoded
# one, and checks dmesg for anything unexpected.
#
# ** Loads and unloads the emlog kernel module (sudo insmod / sudo rmmod). **
#
# Usage: test/run_basic_smoke_test.sh

set -euo pipefail
cd "$(dirname "$0")/.."

DEV=/tmp/emlog_smoke_test
fail=0

cleanup() {
    sudo rm -f "$DEV"
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

DMESG_BEFORE=$(sudo dmesg | wc -l)

echo "== loading module =="
sudo insmod ./emlog.ko

MAJOR=$(awk '$2=="emlog"{print $1}' /proc/devices)
echo "emlog major = $MAJOR"

echo "== /dev/emlog auto-created by udev/devtmpfs =="
if [ -c /dev/emlog ]; then
    echo "  OK: /dev/emlog exists"
    # device_create() leaves it root:root 0600 by default, so this needs sudo.
    if sudo ./emlog_stat /dev/emlog; then
        echo "  OK: /dev/emlog is openable and answers EMLOG_GET_STATUS (regression check for Finding 3)"
    else
        echo "  FAIL: /dev/emlog exists but emlog_stat couldn't query it"
        fail=1
    fi
else
    echo "  (no /dev/emlog -- devtmpfs/udev not active here; not fatal, continuing with an explicit mknod device)"
fi

echo "== manual device: mknod + write + nbcat + emlog_stat =="
sudo mknod "$DEV" c "$MAJOR" 8
sudo chmod 666 "$DEV"

# emlog_autofree defaults to true in this fork (by design -- confirmed):
# the buffer is destroyed as soon as the last fd closes. Hold our own fd
# open across the write+read below so the buffer survives long enough
# for nbcat (a separate open) to see it; this mirrors an overlapping
# writer+reader, not a "write, fully close, read back later" pattern
# (which autofree=true intentionally does not support -- verified below).
exec 3<>"$DEV"
MSG="smoke-test-$RANDOM"
echo "$MSG" >&3

GOT=$(./nbcat "$DEV")
if [ "$GOT" = "$MSG" ]; then
    echo "  OK: nbcat read back exactly what was written (fd still held open)"
else
    echo "  FAIL: nbcat returned [$GOT], expected [$MSG]"
    fail=1
fi

./emlog_stat "$DEV" || { echo "  FAIL: emlog_stat failed"; fail=1; }

exec 3<&-   # drop the last fd -> autofree=true destroys the buffer now
sleep 0.1

echo "== confirming autofree=true actually destroys the buffer once closed =="
AFTER_CLOSE=$(./nbcat "$DEV")
if [ -z "$AFTER_CLOSE" ]; then
    echo "  OK: buffer is empty after the last close, as intended by emlog_autofree=true"
else
    echo "  FAIL: expected an empty buffer after close, got [$AFTER_CLOSE]"
    fail=1
fi

sudo rm -f "$DEV"

echo "== checking dmesg for anything unexpected during the run =="
NEW_LOG=$(sudo dmesg | tail -n "+$((DMESG_BEFORE + 1))")
echo "$NEW_LOG"
if echo "$NEW_LOG" | grep -Ei 'BUG:|Oops|WARNING:|general protection fault|Unable to handle kernel|KASAN|kernel panic'; then
    echo "!! kernel logged something unexpected (see above) !!"
    fail=1
fi

echo "== unloading module =="
sudo rmmod emlog

if [ "$fail" -ne 0 ]; then
    echo "== SMOKE TEST FAILED =="
    exit 1
fi
echo "== SMOKE TEST PASSED =="
