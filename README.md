emlog -- the EMbedded-system LOG-device
=======================================

Version 0.70, 10 July 2018

Author:   Jeremy Elson <jelson@circlemud.org><br/>
Web page:
* http://www.circlemud.org/~jelson/software/emlog
* https://github.com/nicupavel/emlog

--------------------------------------------------------------------------


What is emlog?
==============

emlog is a Linux kernel module that makes it easy to access the most
recent (and *only* the most recent) output from a process.  It works
just like "tail -f" on a log file, except that the storage required
never grows.  This can be useful in embedded systems where there isn't
enough memory or disk space for keeping complete log files, but the
most recent debugging messages are sometimes needed (e.g., after an
error is observed).

The emlog kernel module implements simple character device driver.
The driver acts like a named pipe that has a finite, circular buffer.
The size of the buffer is easily configurable.  As more data is
written into the buffer, the oldest data is discarded.  A process that
reads from an emlog device will first read the existing buffer, then
see new text as it's written, similar to monitoring a log file using
"tail -f".  (Non-blocking reads are also supported, if a process needs
to get the current contents of the log without blocking to wait for
new data.)

The current version of emlog should work under just about any Linux
kernel in the 2.6.x (at least 2.6.32 and newer), 3.x, and
4.x series (at least up to 4.18-rc4).

emlog is free software, distributed under the GNU General Public
License (GPL) version 2; see the file COPYING (in the distribution) for
details.


How is emlog used?
==================

### 1: Configure, compile, and install emlog

   If you want to compile emlog for use with the currently running kernel,
   simply run
   ```bash
   make
   ```

   Otherwise, you have to set either KVER (for linux kernel sources,
   located in `/lib/modules/<KVER>/build`) or KDIR (for any other path):
   ```bash
   make KDIR=/usr/src/linux
   ```

   For cross-compilation (e.g., for ARM targets), specify CROSS_COMPILE:
   ```bash
   make CROSS_COMPILE=arm-linux-gnueabihf-
   ```

   Or combine with KDIR/KVER for kernel module compilation:
   ```bash
   make KDIR=/path/to/kernel ARCH=arm CROSS_COMPILE=arm-linux-gnueabihf-
   ```

   Three files should be generated: the kernel module itself (`emlog.ko`),
   and two utilities (`nbcat` and `emlog_stat`) that will be described later.
   If libfuse is available, `emlog_fuse` will also be built automatically.
   You can use them directly from the current directory or you can install them via
   ```bash
   make install
   ```

#### Building on Raspberry Pi OS

   emlog's whole reason for existing -- a fixed-size log buffer that
   never grows -- is squarely aimed at boards like the Raspberry Pi that
   boot off a microSD card, where you don't want an ordinary log file
   slowly wearing out or filling the card. On Raspberry Pi OS (Debian-based,
   arm64, running the Raspberry Pi Foundation's own kernel rather than a
   generic Debian one) you need the *matching* kernel headers package,
   not Debian's `linux-headers-$(uname -r)`:
   ```bash
   sudo apt-get update
   sudo apt-get install -y raspberrypi-kernel-headers libfuse-dev libcap-dev
   ```
   - `raspberrypi-kernel-headers` comes from the `archive.raspberrypi.org`
     apt source (present by default on Raspberry Pi OS as
     `/etc/apt/sources.list.d/raspi.list`) and must be the same version as
     the currently-running `raspberrypi-kernel` package, since `make`
     builds against `/lib/modules/$(uname -r)/build`. If `insmod` refuses
     the module with "Invalid module format" afterwards, check
     `dmesg` for a `vermagic`/`disagrees about version of symbol
     module_layout` message -- that means the headers and running kernel
     drifted apart (e.g. after a `apt upgrade` that pulled a new kernel
     without rebooting into it yet), not a problem with emlog itself.
   - `libfuse-dev` (not `libfuse3-dev`) and `libcap-dev` are only needed
     if you want `emlog_fuse` (see below); everything else builds without
     them.

   Once those are installed, plain `make` (no `KDIR`/`KVER` override
   needed) builds everything, exactly as above.


### 2: Load the emlog module into the kernel

   If you chose to use emlog directly from the current directly, insert
   the module into the kernel using the `insmod` command
   ```bash
   insmod emlog.ko
   ```

   Otherwise, `modprobe` should work:
   ```bash
   modprobe emlog
   ```

   To specify a different maximum buffer size limit:
   ```bash
   modprobe emlog emlog_max_size=2048
   ```

   By default (`emlog_autofree=1`), a device's buffer is freed as soon
   as no process has it open anymore -- so data does *not* survive
   across separate open/close sessions unless something already has
   the device open when the buffer would otherwise be freed. Load with
   `emlog_autofree=0` if you want buffers to persist across opens (the
   traditional emlog behavior; see step 4 and "Other Usage Notes"
   below for what this means in practice):
   ```bash
   modprobe emlog emlog_autofree=0
   ```

   If successful, a message similar to
   ```
   emlog:emlog_init: version 0.70 running, major is 251, MINOR is 1, max size 1024 K.
   ```
   should show up in your kernel log (type `dmesg` to see it).
   You can also verify that the module has been inserted by
   typing `lsmod` or `cat /proc/modules`.


### 3: Create device files for emlog

   By default, a device file `/dev/emlog` is created (if you have devtmpfs
   mounted onto `/dev` and/or have udev running) with a minimal allocated buffer.
   It's ready to be written to/read from.

   If you need more devices/buffers, you should can use the `mkemlog` program
   to create device files that your processes can write to.

   Usage `mkemlog <logdevname> [size_in_kilobytes] [mode]`

#### 3.1: Examples usage mkemlog

   Create a log file with a 8k buffer with file permissions 0660

   ```bash
   mkemlog /tmp/testlog
   ```

   Create a log file with a 17k buffer with file permissions 0660

   ```bash
   mkemlog /tmp/testlog 17
   ```

   Create a log file with a 12k buffer with file permissions 0644
   ```bash
   mkemlog /tmp/testlog_12k 12 0644
   ```

   Create a log file with a 18k buffer with file permissions 0644, owned by a user with UID==1000.
   ```bash
   mkemlog /tmp/testlog_18k 18 0644 1000
   ```

   The mkemlog requires the `/dev/emlog` file to be created.

#### 3.2: Manually Creating emlogs

   If you do not have devtmpfs mounted onto `/dev` and/or have udev running,
   Then you can manually create emlogs using `mknod` to create device
   files that your processes can write to.
   You need to know two numbers: the major and the minor.
   You can find the major number by either of the following methods:
   ```bash
   ls -l /dev/emlog
   grep emlog /proc/devices
   (source /sys/class/emlog/emlog/uevent ; echo "$MAJOR")
   dmesg | grep emlog
   ```
   The minor number is used to indicate the *size* of the ring
   buffer for that device file, specified as the the number of
   kilobytes (e.g., 1024 bytes).  For example, to create an 8K buffer
   called 'testlog':
   ```bash
   mknod /tmp/testlog c 251 8
   ```

   You can create as many devices as you like.  Internally, emlog uses
   the file's inode and device numbers to identify the buffer to which
   the file refers. Note, that internal buffer size is currently limited to 128K.


### 4: Write to and read from your new device file

   Once the device file has been created, simply write to your device
   file as you would any normal named pipe, e.g.
   ```bash
   echo hello > /tmp/testlog
   ```

   Writes to the log will never block because the buffer never runs
   out of space; old data is simply overwritten by new data.

   **A word of caution**: with the default `emlog_autofree=1`, the
   buffer above is freed the instant `echo`'s shell redirection closes
   the file -- so a *separate*, later `cat /tmp/testlog` command (in a
   new process, after `echo` has already exited) will see an empty,
   freshly-allocated buffer, not "hello". To see data survive across
   separate opens like this, load the module with `emlog_autofree=0`
   (step 2) first.

   With the default settings, the way to see this in action is to
   start the reader *before* the writer closes -- e.g. start `cat`
   first in one terminal:
   ```bash
   cat /tmp/testlog
   _      [blocked, waiting for data -- just like tail -f]
   ```
   ...then, in another terminal:
   ```bash
   echo hello > /tmp/testlog
   ```
   ...and the first terminal immediately shows:
   ```
   hello  [we see the hello that was just written]
   _      [... and here's the cursor.  the 'cat' process is still
           blocked, waiting for new input.  New data will be displayed
           as it is written to the device by other processes.]
   ^C     [use control-c, for example, to stop reading.]
   ```

   As of version 0.40, emlog's buffers can be read and/or monitored
   by multiple concurrent readers correctly.  Data written to an
   emlog device will not disappear until it is overwritten by newer
   data, or the emlog module is removed.  (In versions 0.30 and
   earlier, data was removed from the buffer the first time it was
   read.)


### 5: Remove emlog when you're done

   Type `rmmod emlog` or `modprobe -r emlog` to remove the emlog kernel
   module and free all associated buffers.  This won't work until all emlog
   device files are closed.


Other Usage Notes
=================

* emlog will allocate a fixed-size buffer on behalf of a device file
if one of the following two conditions is true:

  1.  A process has the file open for reading or writing
  2.  A process has written text to the pipe

Whether the buffer survives after the *last* process closes the
device depends on the `emlog_autofree` module parameter (see step 2):
with the default `emlog_autofree=1`, the buffer is freed as soon as
no process has it open anymore. Load the module with
`emlog_autofree=0` if you want buffers to persist across opens (the
traditional emlog behavior) -- in that case, it's possible (naturally)
to fill virtual memory by creating many large emlog devices and
writing one byte to all of them. Don't do that. Regardless of
`emlog_autofree`, all buffers are freed when the emlog kernel module
is removed.

* Non-blocking reads work; i.e., setting O_NONBLOCK using ioctl()
will cause an EAGAIN to be returned if there is no data ready.  In
addition, the select() and poll() functions will work correctly on
emlog devices.

* A small utility, `nbcat`, is included with the emlog distribution.
nbcat is similar to `cat`, but uses nonblocking reads.
This utility can be used to copy the current contents of an emlog
device without blocking to wait for more input.  For example:
   ```bash
   nbcat /var/log/emlog-device-instance > /tmp/saved-log-file
   ```
...will copy the current contents of the named emlog device to a file
in `/tmp`.
Alternatively, you can use `dd` for that
   ```bash
   dd if=/var/log/emlog-device-instance of=/tmp/saved-log-file bs=4096 iflag=nonblock 2>/dev/null
   ```

* A utility `emlog_stat` is included to query buffer status via ioctl.
This utility displays buffer size, current data length, total bytes
written, and the number of open file descriptors.  For example:
   ```bash
   emlog_stat /tmp/testlog
   ```
...will display the current status of the emlog device.  You can query
multiple devices at once:
   ```bash
   emlog_stat /tmp/testlog /dev/emlog
   ```


emlog_fuse
==========

`emlog_fuse` is a FUSE filesystem that transparently redirects file
writes to emlog circular buffer devices.  Any file created under the
mount point is automatically backed by an emlog kernel buffer.
This allows applications to write logs to a regular path without
any code changes, while the actual storage is a fixed-size circular
buffer managed by the emlog kernel module.

### Build requirements

* `libfuse-dev` (libfuse2, FUSE 2.6+)
* `libcap-dev` (optional, for non-root capability checks)

If libfuse is available, `emlog_fuse` is built automatically as part
of `make all`.  If libfuse is not found, it is silently skipped.

### Usage

The emlog kernel module must be loaded before starting `emlog_fuse`.

```bash
# Basic usage (root)
sudo emlog_fuse /var/log/myapp

# Custom buffer size (256 KB per file)
sudo emlog_fuse /var/log/myapp -o buffer_size=256

# Set file ownership by username (resolves uid and gid from passwd)
sudo emlog_fuse /var/log/myapp -o user=myapp

# Set file ownership by numeric uid/gid
sudo emlog_fuse /var/log/myapp -o uid=1000,gid=1000

# Unmount
sudo fusermount -u /var/log/myapp
```

### Non-root usage

`emlog_fuse` requires `CAP_MKNOD` to create emlog device nodes.
`CAP_CHOWN` is also needed if file ownership differs from the
running user.  Grant capabilities with:
```bash
sudo setcap 'cap_mknod,cap_chown+ep' ./emlog_fuse
./emlog_fuse /var/log/myapp -o user=myapp
```

### Options

| Option | Description |
|---|---|
| `-o buffer_size=N` | Buffer size per file in KB (default: 128) |
| `-o dev_dir=PATH` | Directory for backing device files (default: `/dev/.emlog_fuse_devs`) |
| `-o uid=UID` | UID or username for file ownership |
| `-o gid=GID` | GID or group name for file ownership |
| `-o user=NAME` | Sets both uid and gid from passwd entry |
| `--allow-other` | Force enable FUSE `allow_other` |
| `--no-allow-other` | Force disable FUSE `allow_other` |
| `-d` | FUSE debug mode (verbose output) |

When `uid` is set to a non-root user, `allow_other` is enabled
automatically so that the specified user can access the mount.

### How it works

When a file is created or opened under the mount point, `emlog_fuse`
creates a character device node (via `mknod`) backed by the emlog
kernel module.  All writes to the FUSE file are forwarded to this
device.  Reads return the current buffer contents (non-blocking).
When all file descriptors for a file are closed, the backing device
is automatically removed.


Emlog and devtmpfs
==================

By default, emlog creates only one device in `/dev/emlog` (or whereever
your devtmpfs is mounted) with minimal buffer size.
It doesn't make much sense to precreate devices with all possible buffer sizes.
emlog lets you create as many log devices as you like,
anywhere on the filesystem -- the module tells
them apart based on their inode number.  Having a single log device
always exist in a single place (/dev) is much less useful.


Troubleshooting
===============

Q: I'm seeing "I/O error" at a time *other* then when the module is
inserted.

A:  Oops - you've found a bug in emlog.  Please report it.


Q:  When I try to access an emlog device file for reading or writing,
I get the error "no such device".

A:  This probably means either that the emlog kernel module is not
loaded; or, that the major number of the device file does not match
the major number that emlog registered.  To see which major number is
being used by emlog, use any of the following methods:
```bash
grep emlog /proc/devices
(source /sys/class/emlog/emlog/uevent ; echo "$MAJOR")
dmesg | grep emlog
```


Q:  When I try to access an emlog device file for reading or writing,
I get the error "invalid argument".

A:  The *minor* number of the emlog device file must be a number
between 1 and 128, representing the number of kilobytes (1,024 bytes)
that should be used for emlog's ring buffer.  Make sure you're
specifying a valid minor number in your `mknod` statement.  Don't use
0.


Q:  I see "no memory" errors when I try opening new emlog files.

A:  Looks like you're out of virtual memory, sport.


Q:  When I try to remove the emlog driver (`rmmod emlog`), I get the
error "Device or resource busy" or "rmmod: ERROR: Module emlog is in use".

A:  That means a process is currently using an emlog device.  You have
to wait until all processes close all emlog device files until the
driver can be removed.  Try using `lsof` to see which files are in use
by which processes.


Q:  I am trying to save a copy of the current emlog buffer to another
file, by typing `cp /tmp/emlog-test /tmp/saved-log-copy`, but cp just
sits there forever.

A:  `cp` is blocked waiting for more data, just like `cat` does when
used with an emlog device.  Use `nbcat`, the non-blocking cat utility
included with the emlog distribution; for example:
   ```bash
   nbcat /tmp/emlog-test > /tmp/saved-log-copy
   ```


Q:  You've made my computer crash.

A:  Sorry.  If you can reproduce the problem I'll try to fix it.


Known Bugs
==========
 * ~~[Racy einfo allocation/destruction](https://github.com/nicupavel/emlog/issues/10)~~ --
   fixed in this fork. `emlog_open()` used to look up an existing
   einfo, drop the list lock, and only then take a reference; a
   concurrent `emlog_release()` closing the last other fd (freeing the
   einfo, since `emlog_autofree` now defaults to `1`) in that window
   caused a use-after-free. The lookup and reference-taking are now a
   single atomic critical section.
 * [sysfs "cannot create duplicate filename" on module reload](https://github.com/nicupavel/emlog/issues/12) --
   fixed in this fork. `emlog_remove()` was calling `device_destroy()`
   with the wrong `dev_t` (the chrdev region's base minor rather than
   the minor `/dev/emlog` was actually `device_create()`'d with), so
   the device's sysfs entry was never actually removed on unload,
   causing every subsequent `insmod` to fail with `-EEXIST`. Once
   triggered on an unpatched module, the only recovery is a reboot (or
   kexec) -- reloading a patched module afterwards does not retroactively
   clean up the already-leaked sysfs entry from an earlier, unpatched load.


Bug reports, patches, complaints, praise, and submissions of Central
Services Form 27B/6, are welcomed at [Emlog github page](https://github.com/nicupavel/emlog).


Version History
===============
### Version 0.70 (July 10, 2018)
 - Change the default size of /dev/emlog from 1KB to 256KB.
 - Allow emlog devices to be up to 1MB large.
 - Fixes for recent kernel / glibc, fix mkemlog.
 - Allow to specify ownership of the log device.
 - Add support for per e-info rwlock and add more debug.
   (fix nasty reader vs writer race condition)
 - Allow dynamic sizing of emlog_max_size via module_param.

### Version 0.60 (September 25, 2016)
 - Added mkemlog utility.
 - Autofree module option (free associated buffer on last close).
 - Create usable /dev/emlog by default (with non-zero sized buffer).
 - Support for kernels >= 3.19.
 - Dropped support for kernels < 2.6.20.
 - Use pr_err() and friends instead of plain printk().
 - Separate Kbuild file and makefile updates.
 - Cleanup: types, static, etc.
 - README conversion to Markdown syntax.

### Version 0.52 (September 4, 2012)
 - Switched to char device region instead of a misc device.
 - Support for both 2.6.x and 3.x kernels.
 - Proper log-levels for printk().
 - Reindented source code (converted tabs to spaces).

#### Changes by Andreas Neustifter <andreas.neustifter at gmail.com> (September 2, 2012)
 - stability fixes
 - module init and remove rewritten

### Version 0.51 (August 31, 2011)
 - Support for 3.0 kernel.
 - Changed to misc device for auto-creation of /dev/emlog by udev

#### Changes by Andriy Stepanov <stanv at altlinux.ru> (August 31, 2011)
 - fix build on 3.0.3 kernel
 - auto register /dev/emlog by udev

### Version 0.50 (year 2006?)
 - Updated to compile and work with 2.6.x kernels.

#### Changes by Nicu Pavel <npavel at mini-box.com> (August 14, 2006)
 - replaced MODULE_PARM macro with module_param function

#### Changes by Nicu Pavel <npavelat mini-box.com> (June 12, 2006)
 - 2.6 kernel functions update from Darien version.
 - 2.6 Kernel Makefile

#### Changes by Darien Kindlund <kindlund at mitre.org>
 - Modified the emlog code to make it compatible with Linux 2.6 kernels.

### Version 0.40 (August 13, 2001)
 - Concurrent readers and writers are now supported correctly (data is
   not consumed when it is first read, as it was in previous
   versions).
 - emlog's ring buffers now allocated using vmalloc instead of kmalloc
   to avoid locking large blocks of contiguous physical memory.
 - Added MODVERSIONS support
 - Added the 'nbcat' utility - similar to cat, but does not block at
   the end of the data.
 - Bug fix: both device number and inode number are now stored
   internally (instead of only the inode number).  This prevents the
   (unlikely) possibility that emlogs on different filesystems might
   share a single buffer.

### Version 0.30 (March 1, 2001)
 - Now compiles correctly for 2.4 series kernels.
 - select() and poll() now work correctly on emlog devices.
 - Bug fix: all instances should not share one wait queue!

### Version 0.20 (June 14, 2000)
 - Initial public release.


Who wrote emlog, and why?
=========================

Emlog was written by Jeremy Elson <jelson@circlemud.org> at the
University of Southern California's Information Sciences Institute as
part of the SCADDS project <http://www.isi.edu/scadds>.  SCADDS is an
embedded systems research project.  We use small PC/104-bus-based
single-board-PCs using Linux.  We wanted to save the debugging output
from certain processes, but since these things have 16MB of disk space
and 32MB of RAM, keeping complete log files was not an option.  These
tiny nodes do have serial ports running PPP, though, so it's possible
to walk over to a node with a laptop, plug in a serial cable, and then
telnet into the box.  Using emlog, we can always keep the most recent
debug messages from our processes; in case of an error, we can plug in
a debug console and see what went wrong.

This work was supported by DARPA under grant No. DABT63-99-1-0011 as
part of the SCADDS project, and was also made possible in part due to
support from Cisco Systems.
