#!/bin/bash
#
# Regression tests for the emlog_fuse.c fixes:
#   A. unlink-while-open no longer tears down a still-open file's device,
#      and a stale handle can no longer bleed into a later file that
#      reuses its slot.                                     (Finding 2)
#   B. rename() onto an existing open file no longer leaks that file's
#      slot forever -- it's replaced, deferred until the old handle
#      closes if still open.                                (Finding 5)
#   C. closing the last handle on a file no longer destroys its buffer;
#      data survives close+reopen, matching emlog's documented
#      "buffers persist after close" behavior.               (Finding 6)
#
# Requires: emlog kernel module already loaded (see run_kernel_race_test.sh
# or just `sudo insmod emlog.ko`), and either running as root or with
# cap_mknod,cap_chown set on ./emlog_fuse (see README's "Non-root usage").
#
# Usage: test/run_fuse_tests.sh

set -uo pipefail
cd "$(dirname "$0")/.."

MNT=/tmp/emlog_fuse_test_mnt
LOG=/tmp/emlog_fuse_test.log
FUSE_PID=""
fail=0

cleanup() {
    fusermount -u "$MNT" 2>/dev/null || sudo fusermount -u "$MNT" 2>/dev/null || true
    if [ -n "$FUSE_PID" ]; then
        kill "$FUSE_PID" 2>/dev/null || true
        wait "$FUSE_PID" 2>/dev/null || true
    fi
}
trap cleanup EXIT

pass() { echo "  OK: $1"; }
fail_() { echo "  FAIL: $1"; fail=1; }
check_eq() {
    # check_eq <description> <expected> <actual>
    if [ "$3" = "$2" ]; then
        pass "$1"
    else
        fail_ "$1 (expected [$2], got [$3])"
    fi
}

echo "== building =="
make emlog_fuse >/dev/null

MAJOR=$(awk '$2=="emlog"{print $1}' /proc/devices 2>/dev/null || true)
if [ -z "$MAJOR" ]; then
    echo "!! emlog module not loaded -- run: sudo insmod emlog.ko !!"
    exit 1
fi

echo "== mounting emlog_fuse at $MNT =="
mkdir -p "$MNT"
# --allow-other: run via sudo, so emlog_fuse's own "uid != 0 -> auto allow_other"
# heuristic never fires (getuid() is 0 under sudo); force it so this script,
# running unprivileged, can actually access the mount.
sudo ./emlog_fuse "$MNT" -o buffer_size=8 --allow-other > "$LOG" 2>&1 &
FUSE_PID=$!
mounted=0
for _ in $(seq 1 50); do
    if mountpoint -q "$MNT" 2>/dev/null; then
        mounted=1
        break
    fi
    sleep 0.2
done
if [ "$mounted" -ne 1 ]; then
    echo "!! mount did not come up within 10s, see $LOG !!"
    cat "$LOG"
    exit 1
fi
echo "mounted (fuse pid=$FUSE_PID)"

echo
echo "-- test A: unlink-while-open (Finding 2) --"
exec 3> "$MNT/app.log"
echo "before-unlink" >&3
rm "$MNT/app.log"

if [ -e "$MNT/app.log" ]; then
    fail_ "app.log still visible right after unlink"
else
    pass "app.log hidden from namespace after unlink"
fi

if echo "after-unlink-still-open" >&3; then
    pass "write on still-open (unlinked) fd succeeded"
else
    fail_ "write on still-open (unlinked) fd failed -- should still work"
fi

echo "new-file-content" > "$MNT/app.log"
NEWCONTENT=$(cat "$MNT/app.log")
check_eq "recreated app.log contains only the new content (no bleed from stale fd)" \
    "new-file-content" "$NEWCONTENT"

exec 3>&-   # close the stale fd -> triggers deferred cleanup of the old slot
sleep 0.3

echo
echo "-- test B: rename onto an open file (Finding 5) --"
exec 4> "$MNT/B.log"
echo "B-original" >&4
echo "A-original" > "$MNT/A.log"
mv "$MNT/A.log" "$MNT/B.log"

if [ -e "$MNT/A.log" ]; then
    fail_ "A.log still exists after being renamed away"
else
    pass "A.log gone after rename"
fi

NEWB=$(cat "$MNT/B.log")
check_eq "B.log after rename contains the renamed A.log's content" \
    "A-original" "$NEWB"

if echo "old-B-still-open" >&4; then
    pass "write on stale (replaced) B.log fd succeeded"
else
    fail_ "write on stale (replaced) B.log fd failed -- should still work"
fi

exec 4>&-
sleep 0.3

echo
echo "-- test C: buffer persists across close+reopen (Finding 6) --"
echo "persisted-line" > "$MNT/persist.log"
sleep 0.2
PERSISTED=$(cat "$MNT/persist.log")
check_eq "content survives a full close (no fd held open) then a fresh read" \
    "persisted-line" "$PERSISTED"

echo "second-line" >> "$MNT/persist.log"
sleep 0.2
APPENDED=$(cat "$MNT/persist.log")
check_eq "second write after reopen appends rather than starting a fresh empty buffer" \
    "$(printf 'persisted-line\nsecond-line')" "$APPENDED"

echo
echo "== emlog_fuse log excerpts (unlink/rename/cleanup activity) =="
grep -E "unlink|rename|cleanup|deferred" "$LOG" || true

echo
if [ "$fail" -ne 0 ]; then
    echo "== FUSE TESTS FAILED =="
    exit 1
fi
echo "== FUSE TESTS PASSED =="
