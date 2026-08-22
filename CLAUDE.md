# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

emlog is a Linux kernel module (character device driver, `emlog.ko`) that implements a fixed-size circular buffer log device — it behaves like a named pipe with `tail -f` semantics, except storage never grows: new writes overwrite the oldest data instead of failing or blocking. Buffer size is set per-device via the device file's minor number (KB) or via `mkemlog`/`emlog_fuse`. Multiple concurrent readers/writers on the same buffer are supported.

The repo also builds a small set of userspace companions:
- `mkemlog` — creates emlog device files (wraps `mknod` against `/dev/emlog`)
- `nbcat` — non-blocking `cat`, for reading the current buffer contents without blocking for more data
- `emlog_stat` — queries buffer status (size, data length, total bytes written, open-fd refcount) via ioctl
- `emlog_fuse` — a FUSE (2.x) filesystem that transparently backs any file created under its mount point with an emlog device, so apps can write "normal" log files that are actually circular buffers with no code changes

`README.ja.md` is a Japanese translation of `README.md`, kept in sync manually — update both when changing user-facing behavior or docs.

## Build

```bash
make                    # builds emlog.ko against the running kernel, plus nbcat, mkemlog, emlog_stat
make install            # installs the kernel module (via modules_install) and userspace tools into $(DESTDIR)/usr/bin
make clean
```

Kernel module build target: `KVER` defaults to `uname -r`, `KDIR` defaults to `/lib/modules/$(KVER)/build`. Override for a different kernel:

```bash
make KDIR=/usr/src/linux
make CROSS_COMPILE=arm-linux-gnueabihf-
make KDIR=/path/to/kernel ARCH=arm CROSS_COMPILE=arm-linux-gnueabihf-
```

`emlog_fuse` is built automatically only if `pkg-config fuse` (libfuse **2.x**, not fuse3 — the code targets `FUSE_USE_VERSION 26`) is found; it's silently skipped otherwise. Its non-root capability-check support additionally links `libcap` if `pkg-config libcap` is found, else falls back to building with `-DNO_LIBCAP`.

Build wiring lives in `Makefile` (userspace tools + kernel module driver + FUSE/libcap feature detection via pkg-config) and `Kbuild` (just declares `obj-m += emlog.o` for the kernel build system).

There is no automated test suite, but `test/` has a manual regression suite covering the kernel UAF, sysfs-leak-on-reload, and emlog_fuse persistence/unlink/rename fixes — see `test/PROCEDURE.md` before running any of it (it loads/unloads the kernel module via `sudo insmod`/`rmmod`, including a deliberate concurrent-open/close stress test, so treat it as something that could hang or crash the box, not a routine command). `test.sh` at the repo root is now a thin wrapper delegating to `test/run_basic_smoke_test.sh`.

DKMS packaging is defined in `dkms.conf` (`make dkms_install` / `make dkms_remove`) for auto-rebuilding the module across kernel upgrades. DKMS only covers `emlog.ko` — it drives the kernel's own out-of-tree build (`make -C <kernel build dir> M=<source dir>`), not this repo's top-level `Makefile`, so the userspace tools are never built or updated by it; build/install those separately.

`.github/workflows/ci.yml` runs on every push/PR: `build` (kernel module + tools against the runner's own kernel) and `smoke-test` (`test/run_basic_smoke_test.sh`) always run; `build-tools` cross-builds the userspace tools only (no kernel headers needed) for amd64/arm64/armv7/armv6 via QEMU + per-arch Docker images (a Raspberry Pi OS-based image for armv6, since Debian's own armhf targets ARMv7+); `release` (tag pushes only) attaches those as GitHub Release assets — deliberately never `emlog.ko`, since its vermagic ties it to one exact kernel build.

## Architecture

### Kernel module (`emlog.c`, `emlog.h`)

Central data structure is `struct emlog_info` ("einfo"), one per distinct backing buffer, keyed by `(i_ino, i_rdev)` of the device file that first opened it — this is how multiple device file paths pointing at the same underlying inode/device share one circular buffer, and how distinct emlog files get distinct buffers. `einfo` structs are tracked in a singly-linked global list (`emlog_info_list`) protected by `emlog_list_lock` (spinlock for list mutation) plus a per-einfo `rwlock` (protects concurrent reads/writes against the same buffer's `read_point`/`write_point`/data).

Key flow through `emlog.c`:
- `emlog_open` → `get_einfo` looks up an existing buffer by inode/rdev, or `alloc_einfo` creates one sized from the minor number (KB) via `vmalloc` (not `kmalloc`, to avoid needing large contiguous physical memory)
- `emlog_write` → `write_to_emlog` appends into the circular buffer, advancing `write_point` and overwriting old data once full; wakes `read_q`
- `emlog_read` → `read_from_emlog` copies out from `read_point` forward; blocks on `read_q` unless `O_NONBLOCK`
- `emlog_release` → `put_einfo` frees the buffer only if `emlog_autofree` is enabled and refcount hits zero (otherwise buffers persist after close until module removal). `emlog_open` and `emlog_release` both go through the same lock/reference-count discipline (lookup and refcount± must happen in one critical section) — see the UAF fix in `test/PROCEDURE.md`/README Known Bugs if touching this path.
- `emlog_ioctl` implements `EMLOG_GET_STATUS` (defined in `emlog_ioctl.h`, shared between kernel and userspace) for `emlog_stat`
- `emlog_poll` backs `select()`/`poll()` support
- Module params (`module_param`): `emlog_autofree` (bool, default true), `emlog_debug` (bool), `emlog_max_size` (int KB, default 1024, per-buffer cap)
- `emlog_init`/`emlog_remove` register a char device region + `class`/`device` so `/dev/emlog` auto-appears under devtmpfs/udev with a minimal default buffer. Two invariants here worth knowing before touching this code: (1) `class_create()`/`device_create()` return `ERR_PTR()` on failure, not `NULL` — check with `IS_ERR()`; (2) `device_destroy()` must be given the exact `dev_t` that was passed to the matching `device_create()` call (`emlog_default_dev`, not `emlog_dev_type` — they differ), or it silently no-ops and leaks the sysfs entry, breaking every subsequent `insmod` with `-EEXIST` until reboot. Both were real bugs found by actually deploying to a second host; see README's Known Bugs / Version History.

Buffer size selection is unusual: it's encoded in the device file's **minor number** (1–128 for manually-`mknod`'d devices, meaning KB), which is why `mkemlog`/`mknod` and `emlog_max_size` interact the way they do — see README "Emlog and devtmpfs" and "Manually Creating emlogs" sections for the exact semantics before changing device-creation logic.

### `emlog_fuse.c`

Single-file FUSE 2.x filesystem. Maintains its own in-memory table of open logical files (`g_files`) and file handles (`g_fh`), each logical file backed by a real emlog character device node created on demand in a hidden directory (`g_dev_dir`, default `/dev/.emlog_fuse_devs`) via `mknod` (`ensure_device`), using the major number auto-discovered from `/proc/devices` (`get_emlog_major`) and a per-mount buffer size (`-o buffer_size=N` KB). FUSE ops (`emfuse_*`) translate filesystem calls into read/write/open/close against that backing device. A file's `unlinked` flag (set by `emfuse_unlink`/`emfuse_rename` when replacing an existing target) gates actual teardown: `emfuse_release` only calls `cleanup_file` (closes the shared `wfd`, removes the device node) once a file is both unlinked *and* its last handle has closed — an ordinary close never destroys a still-named file's buffer, matching the kernel module's own persist-after-close semantics. `check_capability`/`preflight` gate the `CAP_MKNOD`/`CAP_CHOWN` requirements for non-root operation (compiled out via `-DNO_LIBCAP` when libcap isn't available).

`emlog_ioctl.h` is shared verbatim between the kernel module and userspace tools (`emlog_stat.c`, `emlog_fuse.c`) — it has a `__KERNEL__` vs. userspace header guard, so changes to the status struct must stay ABI-compatible across that boundary.

## Working notes

See `NOTES.md` in the repo root (gitignored, not committed) for a running, append-only log of build environment setup and troubleshooting on this specific host.
