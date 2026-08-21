#!/bin/bash
#
# Regression test for the emlog_open() use-after-free fix (Finding 1).
#
# Hammers open()+write()+close() on a single emlog device from many
# parallel processes so its refcount repeatedly drops to zero (autofree
# defaults to true in this fork), which is exactly the window the old
# code raced on. Checks dmesg for kernel-side errors during the run and
# confirms the module is still cleanly removable afterwards.
#
# ** This script loads and unloads the emlog kernel module (sudo insmod /
#    sudo rmmod). Run it on a machine you're prepared to reboot if
#    something goes wrong -- that's the whole point of the test, but it
#    means a real (if hopefully now-fixed) kernel bug could still hang or
#    crash the box. **
#
# Usage: test/run_kernel_race_test.sh [WORKERS] [ITERS]
# Env overrides: WORKERS (default 16), ITERS (default 20000)

set -euo pipefail
cd "$(dirname "$0")/.."

WORKERS=${1:-${WORKERS:-16}}
ITERS=${2:-${ITERS:-20000}}
DEV=/tmp/emlog_race_test

cleanup() {
    sudo rm -f "$DEV"
    if lsmod | grep -q '^emlog '; then
        sudo rmmod emlog 2>/dev/null || true
    fi
}
trap cleanup EXIT

echo "== building =="
make >/dev/null
gcc -O2 -Wall -o test/stress_open_close test/stress_open_close.c

if lsmod | grep -q '^emlog '; then
    echo "!! emlog module already loaded -- unload it first (sudo rmmod emlog) so this test starts clean !!"
    exit 1
fi

echo "== loading module (emlog_autofree defaults to true) =="
sudo insmod ./emlog.ko

MAJOR=$(awk '$2=="emlog"{print $1}' /proc/devices)
if [ -z "$MAJOR" ]; then
    echo "!! could not find emlog in /proc/devices after insmod !!"
    exit 1
fi
echo "emlog major = $MAJOR"

sudo mknod "$DEV" c "$MAJOR" 8
sudo chmod 666 "$DEV"

DMESG_BEFORE=$(dmesg | wc -l)

echo "== stress: $WORKERS workers x $ITERS open+write+close cycles each =="
pids=()
for _ in $(seq 1 "$WORKERS"); do
    test/stress_open_close "$DEV" "$ITERS" &
    pids+=("$!")
done

fail=0
for pid in "${pids[@]}"; do
    wait "$pid" || fail=1
done

echo "== checking dmesg for new kernel errors =="
NEW_LOG=$(dmesg | tail -n "+$((DMESG_BEFORE + 1))")
if echo "$NEW_LOG" | grep -Ei 'BUG:|Oops|WARNING:|general protection fault|Unable to handle kernel|KASAN|kernel panic'; then
    echo "$NEW_LOG"
    echo "!! kernel logged something during the stress run (see above) !!"
    fail=1
fi

sudo rm -f "$DEV"

echo "== sanity: module still cleanly removable =="
if ! sudo rmmod emlog; then
    echo "!! rmmod failed -- module state likely corrupted by the race !!"
    fail=1
fi

if [ "$fail" -ne 0 ]; then
    echo "== RACE TEST FAILED =="
    exit 1
fi

echo "== RACE TEST PASSED: $((WORKERS * ITERS)) total open/write/close cycles, no kernel errors, clean rmmod =="
