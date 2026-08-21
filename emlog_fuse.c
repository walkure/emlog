/*
 * emlog_fuse.c - FUSE filesystem that transparently redirects ALL file writes
 * to emlog circular buffer devices.
 *
 * For libfuse2 (FUSE 2.6+).
 *
 * Build:
 *   gcc -Wall -O2 emlog_fuse.c -o emlog_fuse \
 *       $(pkg-config fuse --cflags --libs) -lpthread -lcap
 *
 *   Without libcap:
 *   gcc -Wall -O2 -DNO_LIBCAP emlog_fuse.c -o emlog_fuse \
 *       $(pkg-config fuse --cflags --libs) -lpthread
 *
 * Usage:
 *   sudo ./emlog_fuse /var/log/myapp
 *   sudo ./emlog_fuse /var/log/myapp -o buffer_size=256
 *   sudo ./emlog_fuse /var/log/myapp -o user=myapp
 *   sudo ./emlog_fuse /var/log/myapp -o uid=1000,gid=1000
 *   sudo fusermount -u /var/log/myapp
 *
 * Non-root (ELF binary only):
 *   sudo setcap 'cap_mknod,cap_chown+ep' ./emlog_fuse
 *   ./emlog_fuse /var/log/myapp -o user=myapp
 */

#define FUSE_USE_VERSION 26

#include <fuse.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <sys/statvfs.h>
#include <pthread.h>
#include <stddef.h>
#include <syslog.h>
#include <limits.h>
#include <time.h>
#include <pwd.h>
#include <grp.h>

#ifndef NO_LIBCAP
#include <sys/capability.h>
#endif

#include <linux/capability.h>

#include "emlog_ioctl.h"

/* ------------------------------------------------------------------ */
/* Constants                                                           */
/* ------------------------------------------------------------------ */

#define MAX_FILES       256
#define MAX_FH          4096
#define DEFAULT_BUF_KB  128
#define DEFAULT_DEV_DIR "/dev/.emlog_fuse_devs"
#define DEV_PREFIX_LEN  80

/* ------------------------------------------------------------------ */
/* Data structures                                                     */
/* ------------------------------------------------------------------ */

struct emlog_file {
    int      used;
    int      unlinked;       /* name removed but still open; hidden from lookups,
                                 kept alive (and its slot reserved) until refcount hits 0 */
    char     name[NAME_MAX + 1];
    char     dev_path[PATH_MAX];
    int      wfd;            /* shared write fd, -1 if closed */
    int      refcount;
    time_t   created_at;
    uid_t    owner_uid;      /* uid of the process that created this file */
    gid_t    owner_gid;      /* gid of the process that created this file */
};

struct fh_entry {
    int  used;
    int  file_idx;           /* index into g_files[] */
    int  rfd;                /* per-handle read fd, opened lazily. -1 if closed */
};

/* ------------------------------------------------------------------ */
/* Globals                                                             */
/* ------------------------------------------------------------------ */

static struct emlog_file g_files[MAX_FILES];
static struct fh_entry   g_fh[MAX_FH];
static int               g_next_fh = 0;
static pthread_mutex_t   g_lock = PTHREAD_MUTEX_INITIALIZER;

static int    g_emlog_major    = -1;
static int    g_buffer_size_kb = DEFAULT_BUF_KB;
static char   g_dev_dir[PATH_MAX] = DEFAULT_DEV_DIR;
static char   g_dev_prefix[DEV_PREFIX_LEN] = "";
static uid_t  g_file_uid = 0;
static gid_t  g_file_gid = 0;

/* ------------------------------------------------------------------ */
/* Capability check                                                    */
/* ------------------------------------------------------------------ */

static int check_capability(int cap)
{
#ifndef NO_LIBCAP
    cap_t caps = cap_get_proc();
    if (caps) {
        cap_flag_value_t val = CAP_CLEAR;
        if (cap_get_flag(caps, (cap_value_t)cap, CAP_EFFECTIVE, &val) == 0) {
            cap_free(caps);
            return val == CAP_SET;
        }
        cap_free(caps);
    }
#endif
    /* fallback: parse /proc/self/status CapEff */
    FILE *f = fopen("/proc/self/status", "r");
    if (!f) return 0;

    char line[256];
    while (fgets(line, sizeof(line), f)) {
        if (strncmp(line, "CapEff:", 7) == 0) {
            unsigned long long cap_eff = 0;
            if (sscanf(line + 7, " %llx", &cap_eff) == 1) {
                fclose(f);
                return (cap_eff & (1ULL << cap)) != 0;
            }
        }
    }
    fclose(f);
    return 0;
}

/* ------------------------------------------------------------------ */
/* UID/GID resolution                                                  */
/* ------------------------------------------------------------------ */

/* Accept numeric string or username. Returns 0 on success. */
static int resolve_uid(const char *s, uid_t *out)
{
    char *end;
    unsigned long v = strtoul(s, &end, 10);
    if (*end == '\0') {
        *out = (uid_t)v;
        return 0;
    }
    struct passwd *pw = getpwnam(s);
    if (pw) {
        *out = pw->pw_uid;
        return 0;
    }
    return -1;
}

/* Accept numeric string or group name. Returns 0 on success. */
static int resolve_gid(const char *s, gid_t *out)
{
    char *end;
    unsigned long v = strtoul(s, &end, 10);
    if (*end == '\0') {
        *out = (gid_t)v;
        return 0;
    }
    struct group *gr = getgrnam(s);
    if (gr) {
        *out = gr->gr_gid;
        return 0;
    }
    return -1;
}

/* Resolve username to uid and gid from passwd. */
static int resolve_user(const char *name, uid_t *uid, gid_t *gid)
{
    struct passwd *pw = getpwnam(name);
    if (!pw) {
        char *end;
        unsigned long v = strtoul(name, &end, 10);
        if (*end != '\0') return -1;
        pw = getpwuid((uid_t)v);
        if (!pw) return -1;
    }
    *uid = pw->pw_uid;
    *gid = pw->pw_gid;
    return 0;
}

/* ------------------------------------------------------------------ */
/* emlog detection                                                     */
/* ------------------------------------------------------------------ */

static int get_emlog_major(void)
{
    FILE *f = fopen("/proc/devices", "r");
    if (!f) return -1;

    char line[256];
    int major = -1;
    while (fgets(line, sizeof(line), f)) {
        int m;
        char name[64];
        if (sscanf(line, " %d %63s", &m, name) == 2) {
            if (strcmp(name, "emlog") == 0) {
                major = m;
                break;
            }
        }
    }
    fclose(f);
    return major;
}

static int get_emlog_max_size(void)
{
    FILE *f = fopen("/sys/module/emlog/parameters/emlog_max_size", "r");
    if (!f) return -1;

    int val = -1;
    if (fscanf(f, "%d", &val) != 1)
        val = -1;
    fclose(f);
    return val;
}

/* ------------------------------------------------------------------ */
/* Device prefix (mountpoint hash for collision avoidance)             */
/* ------------------------------------------------------------------ */
static void build_dev_prefix(const char *mountpoint)
{
    char abspath[PATH_MAX];
    if (realpath(mountpoint, abspath) == NULL) {
        strncpy(abspath, mountpoint, sizeof(abspath) - 1);
        abspath[sizeof(abspath) - 1] = '\0';
    }

    const char *base = strrchr(abspath, '/');
    base = base ? base + 1 : abspath;
    if (*base == '\0') base = "root";

    /* djb2 hash */
    unsigned long h = 5381;
    for (const char *p = abspath; *p; p++)
        h = ((h << 5) + h) ^ (unsigned char)*p;

    /* truncate basename to fit the prefix buffer */
    char base_trunc[61];
    size_t len = strlen(base);
    if (len >= sizeof(base_trunc))
        len = sizeof(base_trunc) - 1;
    memcpy(base_trunc, base, len);
    base_trunc[len] = '\0';

    snprintf(g_dev_prefix, sizeof(g_dev_prefix),
             "%s_%08lx", base_trunc, h & 0xffffffffUL);
}

/* ------------------------------------------------------------------ */
/* File / fh management (caller must hold g_lock)                      */
/* ------------------------------------------------------------------ */

static int find_file(const char *name)
{
    for (int i = 0; i < MAX_FILES; i++) {
        if (g_files[i].used && !g_files[i].unlinked &&
            strcmp(g_files[i].name, name) == 0)
            return i;
    }
    return -1;
}

static int alloc_file_slot(void)
{
    for (int i = 0; i < MAX_FILES; i++) {
        if (!g_files[i].used)
            return i;
    }
    return -1;
}

static int alloc_fh(int file_idx)
{
    for (int i = 0; i < MAX_FH; i++) {
        int idx = (g_next_fh + i) % MAX_FH;
        if (!g_fh[idx].used) {
            g_fh[idx].used     = 1;
            g_fh[idx].file_idx = file_idx;
            g_fh[idx].rfd      = -1;
            g_next_fh = (idx + 1) % MAX_FH;
            syslog(LOG_DEBUG, "alloc fh=%d -> '%s'",
                   idx, g_files[file_idx].name);
            return idx;
        }
    }
    return -1;
}

static void dev_path_for(const char *name, char *out, size_t outlen)
{
    char safe[NAME_MAX + 1];
    snprintf(safe, sizeof(safe), "%s", name);
    for (char *p = safe; *p; p++) {
        if (*p == '/') *p = '_';
    }

    int n = snprintf(out, outlen, "%s/%s__%s", g_dev_dir, g_dev_prefix, safe);
    if (n >= (int)outlen)
        syslog(LOG_WARNING, "dev_path truncated: %s", out);
}

static int ensure_device(const char *name)
{
    int idx = find_file(name);
    if (idx >= 0)
        return idx;

    idx = alloc_file_slot();
    if (idx < 0)
        return -ENOMEM;

    struct emlog_file *ef = &g_files[idx];
    memset(ef, 0, sizeof(*ef));
    ef->wfd = -1;

    dev_path_for(name, ef->dev_path, sizeof(ef->dev_path));

    /* remove stale device file */
    struct stat st;
    if (stat(ef->dev_path, &st) == 0) {
        syslog(LOG_WARNING, "stale device, removing: %s", ef->dev_path);
        unlink(ef->dev_path);
    }

    dev_t dev = makedev(g_emlog_major, g_buffer_size_kb);
    syslog(LOG_INFO, "mknod %s (major=%d, minor=%d)",
           ef->dev_path, g_emlog_major, g_buffer_size_kb);
    if (mknod(ef->dev_path, S_IFCHR | 0666, dev) < 0) {
        syslog(LOG_ERR, "mknod %s failed: %s", ef->dev_path, strerror(errno));
        return -EIO;
    }

    struct fuse_context *fc = fuse_get_context();
    uid_t owner_uid = fc ? fc->uid : g_file_uid;
    gid_t owner_gid = fc ? fc->gid : g_file_gid;

    if (chown(ef->dev_path, owner_uid, owner_gid) < 0)
        syslog(LOG_WARNING, "chown %s to %d:%d failed: %s",
               ef->dev_path, owner_uid, owner_gid, strerror(errno));

    strncpy(ef->name, name, NAME_MAX);
    ef->name[NAME_MAX] = '\0';
    ef->refcount   = 0;
    ef->created_at = time(NULL);
    ef->owner_uid  = owner_uid;
    ef->owner_gid  = owner_gid;
    ef->used       = 1;

    syslog(LOG_INFO, "created: name='%s' dev='%s'", ef->name, ef->dev_path);
    return idx;
}

static int ensure_wfd(struct emlog_file *ef)
{
    if (ef->wfd >= 0)
        return 0;

    ef->wfd = open(ef->dev_path, O_WRONLY);
    if (ef->wfd < 0) {
        syslog(LOG_ERR, "open(%s, O_WRONLY) failed: %s",
               ef->dev_path, strerror(errno));
        return -EIO;
    }
    syslog(LOG_INFO, "opened wfd=%d for %s", ef->wfd, ef->dev_path);
    return 0;
}

static void cleanup_file(int idx)
{
    struct emlog_file *ef = &g_files[idx];
    if (!ef->used) return;

    syslog(LOG_INFO, "cleaning up '%s'", ef->name);

    if (ef->wfd >= 0) {
        close(ef->wfd);
        ef->wfd = -1;
    }

    struct stat st;
    if (stat(ef->dev_path, &st) == 0)
        unlink(ef->dev_path);

    ef->used     = 0;
    ef->unlinked = 0;
    syslog(LOG_INFO, "cleanup complete for '%s'", ef->name);
}

static int query_emlog_status(const char *dev_path, struct emlog_status *st)
{
    int fd = open(dev_path, O_RDONLY | O_NONBLOCK);
    if (fd < 0) return -1;
    int ret = ioctl(fd, EMLOG_GET_STATUS, st);
    close(fd);
    return ret;
}

/* ------------------------------------------------------------------ */
/* FUSE operations (libfuse2 API)                                      */
/* ------------------------------------------------------------------ */

static int emfuse_getattr(const char *path, struct stat *stbuf)
{
    memset(stbuf, 0, sizeof(*stbuf));

    if (strcmp(path, "/") == 0) {
        stbuf->st_mode  = S_IFDIR | 0755;
        stbuf->st_nlink = 2;
        stbuf->st_uid   = g_file_uid;
        stbuf->st_gid   = g_file_gid;
        stbuf->st_atime = stbuf->st_mtime = stbuf->st_ctime = time(NULL);
        return 0;
    }

    const char *name = path + 1;
    if (strchr(name, '/'))
        return -ENOENT;

    pthread_mutex_lock(&g_lock);
    int idx = find_file(name);
    if (idx < 0) {
        pthread_mutex_unlock(&g_lock);
        return -ENOENT;
    }

    struct emlog_file *ef = &g_files[idx];
    stbuf->st_mode  = S_IFREG | 0644;
    stbuf->st_nlink = 1;
    stbuf->st_size  = 0;
    stbuf->st_uid   = ef->owner_uid;
    stbuf->st_gid   = ef->owner_gid;
    stbuf->st_atime = ef->created_at;
    stbuf->st_mtime = ef->created_at;
    stbuf->st_ctime = ef->created_at;

    struct emlog_status st;
    if (query_emlog_status(ef->dev_path, &st) == 0)
        stbuf->st_size = st.data_len;

    pthread_mutex_unlock(&g_lock);
    return 0;
}

static int emfuse_readdir(const char *path, void *buf, fuse_fill_dir_t filler,
                          off_t offset, struct fuse_file_info *fi)
{
    (void)offset; (void)fi;

    if (strcmp(path, "/") != 0)
        return -ENOENT;

    filler(buf, ".",  NULL, 0);
    filler(buf, "..", NULL, 0);

    pthread_mutex_lock(&g_lock);
    for (int i = 0; i < MAX_FILES; i++) {
        if (g_files[i].used && !g_files[i].unlinked)
            filler(buf, g_files[i].name, NULL, 0);
    }
    pthread_mutex_unlock(&g_lock);
    return 0;
}

static int emfuse_statfs(const char *path, struct statvfs *stbuf)
{
    (void)path;
    memset(stbuf, 0, sizeof(*stbuf));
    stbuf->f_bsize   = 4096;
    stbuf->f_frsize  = 4096;
    stbuf->f_blocks  = 1024;
    stbuf->f_bfree   = 1024;
    stbuf->f_bavail  = 1024;
    stbuf->f_files   = 1024;
    stbuf->f_ffree   = 1024;
    stbuf->f_favail  = 1024;
    stbuf->f_namemax = 255;
    return 0;
}

static int emfuse_create(const char *path, mode_t mode,
                         struct fuse_file_info *fi)
{
    (void)mode;
    const char *name = path + 1;
    syslog(LOG_INFO, "create('%s', mode=0%o)", name, mode);

    pthread_mutex_lock(&g_lock);

    int idx = ensure_device(name);
    if (idx < 0) {
        pthread_mutex_unlock(&g_lock);
        return idx;
    }

    struct emlog_file *ef = &g_files[idx];
    int ret = ensure_wfd(ef);
    if (ret < 0) {
        pthread_mutex_unlock(&g_lock);
        return ret;
    }

    ef->refcount++;
    int fh = alloc_fh(idx);
    if (fh < 0) {
        ef->refcount--;
        pthread_mutex_unlock(&g_lock);
        return -ENOMEM;
    }

    fi->fh = (uint64_t)fh;
    syslog(LOG_INFO, "create('%s') -> fh=%d refcount=%d",
           name, fh, ef->refcount);

    pthread_mutex_unlock(&g_lock);
    return 0;
}

static int emfuse_open(const char *path, struct fuse_file_info *fi)
{
    const char *name = path + 1;
    syslog(LOG_INFO, "open('%s', flags=0x%x)", name, fi->flags);

    pthread_mutex_lock(&g_lock);

    int idx = ensure_device(name);
    if (idx < 0) {
        pthread_mutex_unlock(&g_lock);
        return idx;
    }

    struct emlog_file *ef = &g_files[idx];
    int ret = ensure_wfd(ef);
    if (ret < 0) {
        pthread_mutex_unlock(&g_lock);
        return ret;
    }

    ef->refcount++;
    int fh = alloc_fh(idx);
    if (fh < 0) {
        ef->refcount--;
        pthread_mutex_unlock(&g_lock);
        return -ENOMEM;
    }

    fi->fh = (uint64_t)fh;
    syslog(LOG_INFO, "open('%s') -> fh=%d refcount=%d",
           name, fh, ef->refcount);

    pthread_mutex_unlock(&g_lock);
    return 0;
}

static int emfuse_write(const char *path, const char *buf, size_t size,
                        off_t offset, struct fuse_file_info *fi)
{
    (void)path; (void)offset;
    int fh = (int)fi->fh;

    if (fh < 0 || fh >= MAX_FH)
        return -EBADF;

    pthread_mutex_lock(&g_lock);
    if (!g_fh[fh].used) {
        syslog(LOG_ERR, "write: unknown fh=%d", fh);
        pthread_mutex_unlock(&g_lock);
        return -EBADF;
    }
    int file_idx = g_fh[fh].file_idx;
    struct emlog_file *ef = &g_files[file_idx];
    if (!ef->used || ef->wfd < 0) {
        syslog(LOG_ERR, "write: no device for '%s' (fh=%d)", ef->name, fh);
        pthread_mutex_unlock(&g_lock);
        return -EBADF;
    }
    int wfd = ef->wfd;
    pthread_mutex_unlock(&g_lock);

    ssize_t n = write(wfd, buf, size);
    if (n < 0) {
        syslog(LOG_ERR, "write: failed wfd=%d: %s", wfd, strerror(errno));
        return -errno;
    }
    return (int)n;
}

/*
 * Read: use a per-handle read fd opened with O_RDONLY|O_NONBLOCK.
 * The fd is opened lazily on first read and kept open until release.
 * This preserves emlog's internal file offset across consecutive reads,
 * so sequential reads (e.g. cat) correctly return successive data
 * rather than re-reading from the buffer start each time.
 */
static int emfuse_read(const char *path, char *buf, size_t size,
                       off_t offset, struct fuse_file_info *fi)
{
    (void)path; (void)offset;
    int fh = (int)fi->fh;

    if (fh < 0 || fh >= MAX_FH)
        return -EBADF;

    pthread_mutex_lock(&g_lock);
    if (!g_fh[fh].used) {
        syslog(LOG_ERR, "read: unknown fh=%d", fh);
        pthread_mutex_unlock(&g_lock);
        return -EBADF;
    }
    int file_idx = g_fh[fh].file_idx;
    struct emlog_file *ef = &g_files[file_idx];
    if (!ef->used) {
        pthread_mutex_unlock(&g_lock);
        return -EBADF;
    }

    /* open read fd lazily on first read */
    if (g_fh[fh].rfd < 0) {
        g_fh[fh].rfd = open(ef->dev_path, O_RDONLY | O_NONBLOCK);
        if (g_fh[fh].rfd < 0) {
            syslog(LOG_ERR, "read: open(%s) failed: %s",
                   ef->dev_path, strerror(errno));
            pthread_mutex_unlock(&g_lock);
            return -errno;
        }
    }
    int rfd = g_fh[fh].rfd;
    pthread_mutex_unlock(&g_lock);

    ssize_t n = read(rfd, buf, size);
    if (n < 0) {
        if (errno == EAGAIN)
            return 0;
        syslog(LOG_ERR, "read: failed: %s", strerror(errno));
        return -errno;
    }
    return (int)n;
}

static int emfuse_release(const char *path, struct fuse_file_info *fi)
{
    int fh = (int)fi->fh;
    syslog(LOG_INFO, "release('%s', fh=%d)", path, fh);

    if (fh < 0 || fh >= MAX_FH)
        return 0;

    pthread_mutex_lock(&g_lock);
    if (!g_fh[fh].used) {
        syslog(LOG_WARNING, "release: unknown fh=%d", fh);
        pthread_mutex_unlock(&g_lock);
        return 0;
    }

    int file_idx = g_fh[fh].file_idx;

    if (g_fh[fh].rfd >= 0) {
        close(g_fh[fh].rfd);
        g_fh[fh].rfd = -1;
    }
    g_fh[fh].used = 0;

    if (file_idx < 0 || file_idx >= MAX_FILES || !g_files[file_idx].used) {
        syslog(LOG_WARNING, "release: no file info for fh=%d", fh);
        pthread_mutex_unlock(&g_lock);
        return 0;
    }

    struct emlog_file *ef = &g_files[file_idx];
    ef->refcount--;
    syslog(LOG_INFO, "release: '%s' refcount -> %d", ef->name, ef->refcount);

    /* Closing the last handle must NOT destroy the buffer: emlog buffers
     * persist across close by design (see the kernel module's own
     * "buffers are persistent, even after a process closes the emlog
     * device" semantics), so a later open() of the same name should see
     * the same history. wfd is deliberately left open so the buffer
     * stays referenced. Only actually tear it down once the name has
     * been removed via unlink()/rename()-replace *and* the last handle
     * on it is gone. */
    if (ef->unlinked && ef->refcount <= 0)
        cleanup_file(file_idx);

    pthread_mutex_unlock(&g_lock);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Misc operations (no-ops / ENOSYS)                                   */
/* ------------------------------------------------------------------ */

static int emfuse_truncate(const char *path, off_t size)
{
    (void)path; (void)size;
    return 0;
}

static int emfuse_ftruncate(const char *path, off_t size,
                            struct fuse_file_info *fi)
{
    (void)path; (void)size; (void)fi;
    return 0;
}

static int emfuse_flush(const char *path, struct fuse_file_info *fi)
{
    (void)path; (void)fi;
    return 0;
}

static int emfuse_fsync(const char *path, int datasync,
                        struct fuse_file_info *fi)
{
    (void)path; (void)datasync; (void)fi;
    return 0;
}

static int emfuse_unlink(const char *path)
{
    const char *name = path + 1;
    syslog(LOG_INFO, "unlink('%s')", name);

    pthread_mutex_lock(&g_lock);
    int idx = find_file(name);
    if (idx < 0) {
        pthread_mutex_unlock(&g_lock);
        return -ENOENT;
    }

    struct emlog_file *ef = &g_files[idx];
    if (ef->refcount > 0) {
        /* still open elsewhere: hide the name (find_file/readdir will
         * skip it) but keep the slot and backing device reserved for
         * this file until the last open handle is released via
         * emfuse_release(), matching POSIX unlink-while-open semantics.
         * This also prevents a later create() of a same-named file from
         * ever sharing this (soon-to-be-stale) slot with the handles
         * still open on it. */
        ef->unlinked = 1;
        syslog(LOG_INFO, "unlink('%s'): deferred, refcount=%d",
               name, ef->refcount);
    } else {
        cleanup_file(idx);
    }
    pthread_mutex_unlock(&g_lock);
    return 0;
}

static int emfuse_rename(const char *from, const char *to)
{
    const char *old_name = from + 1;
    const char *new_name = to + 1;
    syslog(LOG_INFO, "rename('%s' -> '%s')", old_name, new_name);

    pthread_mutex_lock(&g_lock);
    int idx = find_file(old_name);
    if (idx < 0) {
        pthread_mutex_unlock(&g_lock);
        return -ENOENT;
    }

    /* if the destination name already refers to a different file,
     * replace it the way POSIX rename() does: drop that name, deferring
     * actual cleanup if it's still open (same as unlink()), instead of
     * leaving it permanently unreachable-but-alive. */
    int target_idx = find_file(new_name);
    if (target_idx >= 0 && target_idx != idx) {
        struct emlog_file *tf = &g_files[target_idx];
        if (tf->refcount > 0) {
            tf->unlinked = 1;
            syslog(LOG_INFO, "rename: replaced '%s' deferred, refcount=%d",
                   new_name, tf->refcount);
        } else {
            cleanup_file(target_idx);
        }
    }

    strncpy(g_files[idx].name, new_name, NAME_MAX);
    g_files[idx].name[NAME_MAX] = '\0';
    pthread_mutex_unlock(&g_lock);
    return 0;
}

static int emfuse_chmod(const char *path, mode_t mode)
{
    (void)path; (void)mode;
    return 0;
}

static int emfuse_chown(const char *path, uid_t uid, gid_t gid)
{
    (void)path; (void)uid; (void)gid;
    return 0;
}

static int emfuse_utimens(const char *path, const struct timespec tv[2])
{
    (void)path; (void)tv;
    return 0;
}

static int emfuse_lock(const char *path, struct fuse_file_info *fi,
                       int cmd, struct flock *lk)
{
    (void)path; (void)fi; (void)cmd; (void)lk;
    return -ENOSYS;
}

/* ------------------------------------------------------------------ */
/* fuse_operations table                                               */
/* ------------------------------------------------------------------ */

static struct fuse_operations emfuse_ops = {
    .getattr   = emfuse_getattr,
    .readdir   = emfuse_readdir,
    .create    = emfuse_create,
    .open      = emfuse_open,
    .read      = emfuse_read,
    .write     = emfuse_write,
    .release   = emfuse_release,
    .unlink    = emfuse_unlink,
    .rename    = emfuse_rename,
    .truncate  = emfuse_truncate,
    .ftruncate = emfuse_ftruncate,
    .flush     = emfuse_flush,
    .fsync     = emfuse_fsync,
    .chmod     = emfuse_chmod,
    .chown     = emfuse_chown,
    .utimens   = emfuse_utimens,
    .lock      = emfuse_lock,
    .statfs    = emfuse_statfs,
    .flag_nullpath_ok = 0,
};

/* ------------------------------------------------------------------ */
/* Option parsing                                                      */
/* ------------------------------------------------------------------ */

struct emfuse_opts {
    int   buffer_size;
    char *dev_dir;
    char *uid_str;
    char *gid_str;
    char *user_str;
    int   force_allow_other;
    int   force_no_allow_other;
    int   show_help;
};

#define EMFUSE_OPT(t, p, v) { t, offsetof(struct emfuse_opts, p), v }

static const struct fuse_opt emfuse_opts_spec[] = {
    EMFUSE_OPT("buffer_size=%d",   buffer_size,          0),
    EMFUSE_OPT("dev_dir=%s",       dev_dir,              0),
    EMFUSE_OPT("uid=%s",           uid_str,              0),
    EMFUSE_OPT("gid=%s",           gid_str,              0),
    EMFUSE_OPT("user=%s",          user_str,             0),
    EMFUSE_OPT("--allow-other",    force_allow_other,    1),
    EMFUSE_OPT("--no-allow-other", force_no_allow_other, 1),
    EMFUSE_OPT("-h",               show_help,            1),
    EMFUSE_OPT("--help",           show_help,            1),
    FUSE_OPT_END
};

static void usage(const char *prog)
{
    fprintf(stderr,
        "Usage: %s <mountpoint> [options]\n"
        "\n"
        "emlog_fuse options:\n"
        "    -o buffer_size=N      Buffer size per file in KB (default: %d)\n"
        "    -o dev_dir=PATH       Device file directory (default: %s)\n"
        "    -o uid=UID_OR_NAME    UID or username for file ownership\n"
        "    -o gid=GID_OR_NAME    GID or group name for file ownership\n"
        "    -o user=USERNAME      Sets both uid and gid from passwd\n"
        "    --allow-other         Force enable allow_other\n"
        "    --no-allow-other      Force disable allow_other\n"
        "    -d                    FUSE debug mode\n"
        "\n"
        "allow_other is automatically enabled when uid != 0.\n"
        "\n"
        "Non-root usage:\n"
        "    sudo setcap 'cap_mknod,cap_chown+ep' %s\n"
        "\n"
        "Examples:\n"
        "    sudo %s /var/log/myapp\n"
        "    sudo %s /var/log/myapp -o buffer_size=256\n"
        "    sudo %s /var/log/myapp -o user=pfclient\n"
        "    sudo %s /var/log/myapp -o uid=1000,gid=1000\n"
        "    sudo fusermount -u /var/log/myapp\n"
        "\n",
        prog, DEFAULT_BUF_KB, DEFAULT_DEV_DIR,
        prog, prog, prog, prog, prog);
}

/* ------------------------------------------------------------------ */
/* Preflight checks                                                    */
/* ------------------------------------------------------------------ */

static int preflight(int needs_chown)
{
    fprintf(stderr, "=== Preflight checks ===\n");

    int is_root = (geteuid() == 0);

    if (is_root) {
        fprintf(stderr, "Running as root (euid=0)\n");
    } else {
        fprintf(stderr, "Running as non-root (euid=%d) — checking capabilities...\n",
                geteuid());
        int ok = 1;

        if (check_capability(CAP_MKNOD))
            fprintf(stderr, "  CAP_MKNOD: OK\n");
        else {
            fprintf(stderr, "  CAP_MKNOD: MISSING — required for mknod\n");
            ok = 0;
        }

        if (needs_chown) {
            if (check_capability(CAP_CHOWN))
                fprintf(stderr, "  CAP_CHOWN: OK\n");
            else
                fprintf(stderr, "  CAP_CHOWN: MISSING — chown may fail (warning)\n");
            /* CAP_CHOWN is non-fatal */
        }

        if (!ok) {
            fprintf(stderr, "\nFix with:\n");
            if (needs_chown)
                fprintf(stderr, "  sudo setcap 'cap_mknod,cap_chown+ep' ./emlog_fuse\n");
            else
                fprintf(stderr, "  sudo setcap 'cap_mknod+ep' ./emlog_fuse\n");
            fprintf(stderr, "  or run as root\n");
            return -1;
        }
    }

    /* emlog module */
    g_emlog_major = get_emlog_major();
    if (g_emlog_major < 0) {
        fprintf(stderr, "ERROR: emlog kernel module is NOT loaded.\n");
        fprintf(stderr, "  Load with: sudo insmod emlog.ko\n");
        return -1;
    }
    fprintf(stderr, "emlog major number: %d\n", g_emlog_major);

    int max_size = get_emlog_max_size();
    if (max_size > 0) {
        fprintf(stderr, "emlog_max_size: %d KB\n", max_size);
        if (g_buffer_size_kb > max_size) {
            fprintf(stderr, "ERROR: buffer_size %d KB > emlog_max_size %d KB\n",
                    g_buffer_size_kb, max_size);
            return -1;
        }
    }

    fprintf(stderr, "=== Preflight OK  major=%d  buffer=%dKB ===\n",
            g_emlog_major, g_buffer_size_kb);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Find mountpoint from fuse_args                                      */
/* ------------------------------------------------------------------ */

static const char *find_mountpoint(struct fuse_args *args)
{
    for (int i = 1; i < args->argc; i++) {
        if (args->argv[i][0] != '-')
            return args->argv[i];
    }
    return NULL;
}

/* ------------------------------------------------------------------ */
/* main                                                                */
/* ------------------------------------------------------------------ */

int main(int argc, char *argv[])
{
    struct fuse_args args = FUSE_ARGS_INIT(argc, argv);
    struct emfuse_opts opts = {
        .buffer_size          = DEFAULT_BUF_KB,
        .dev_dir              = NULL,
        .uid_str              = NULL,
        .gid_str              = NULL,
        .user_str             = NULL,
        .force_allow_other    = 0,
        .force_no_allow_other = 0,
        .show_help            = 0,
    };

    if (fuse_opt_parse(&args, &opts, emfuse_opts_spec, NULL) == -1)
        return 1;

    if (opts.show_help) {
        usage(argv[0]);
        fuse_opt_add_arg(&args, "--help");
        fuse_main(args.argc, args.argv, &emfuse_ops, NULL);
        return 0;
    }

    g_buffer_size_kb = opts.buffer_size;
    if (opts.dev_dir)
        strncpy(g_dev_dir, opts.dev_dir, sizeof(g_dev_dir) - 1);

    /* --- Resolve uid/gid --- */
    g_file_uid = getuid();
    g_file_gid = getgid();

    if (opts.user_str) {
        if (resolve_user(opts.user_str, &g_file_uid, &g_file_gid) < 0) {
            fprintf(stderr, "ERROR: unknown user: '%s'\n", opts.user_str);
            return 1;
        }
    }
    if (opts.uid_str) {
        if (resolve_uid(opts.uid_str, &g_file_uid) < 0) {
            fprintf(stderr, "ERROR: unknown uid: '%s'\n", opts.uid_str);
            return 1;
        }
    }
    if (opts.gid_str) {
        if (resolve_gid(opts.gid_str, &g_file_gid) < 0) {
            fprintf(stderr, "ERROR: unknown gid: '%s'\n", opts.gid_str);
            return 1;
        }
    }

    /* --- allow_other --- */
    int allow_other;
    if (opts.force_no_allow_other)
        allow_other = 0;
    else if (opts.force_allow_other)
        allow_other = 1;
    else
        allow_other = (g_file_uid != 0);

    int needs_chown = allow_other ||
                      (g_file_uid != (uid_t)geteuid() ||
                       g_file_gid != (gid_t)getegid());

    /* --- mountpoint --- */
    const char *mountpoint = find_mountpoint(&args);
    if (!mountpoint) {
        fprintf(stderr, "ERROR: no mountpoint specified.\n");
        usage(argv[0]);
        return 1;
    }
    build_dev_prefix(mountpoint);

    /* --- foreground fixed --- */
    fuse_opt_add_arg(&args, "-f");

    if (allow_other)
        fuse_opt_add_arg(&args, "-oallow_other");

    /* --- logging --- */
    openlog("emlog_fuse", LOG_PID | LOG_PERROR, LOG_DAEMON);

    fprintf(stderr, "emlog_fuse starting  pid=%d  euid=%d\n",
            getpid(), geteuid());
    fprintf(stderr, "  mountpoint   = %s\n", mountpoint);
    fprintf(stderr, "  buffer_size  = %d KB\n", g_buffer_size_kb);
    fprintf(stderr, "  dev_dir      = %s\n", g_dev_dir);
    fprintf(stderr, "  dev_prefix   = %s\n", g_dev_prefix);
    fprintf(stderr, "  file_uid     = %d\n", g_file_uid);
    fprintf(stderr, "  file_gid     = %d\n", g_file_gid);
    fprintf(stderr, "  allow_other  = %s%s\n",
            allow_other ? "true" : "false",
            (!opts.force_allow_other && !opts.force_no_allow_other && allow_other)
                ? " (auto: uid != 0)" : "");

    /* --- init --- */
    memset(g_files, 0, sizeof(g_files));
    memset(g_fh,    0, sizeof(g_fh));

    if (preflight(needs_chown) < 0) {
        closelog();
        return 1;
    }

    mkdir(g_dev_dir, 0755);

    /* create + chown mountpoint */
    mkdir(mountpoint, 0755);
    if (chown(mountpoint, g_file_uid, g_file_gid) < 0)
        fprintf(stderr, "WARNING: chown mountpoint failed: %s\n",
                strerror(errno));

    /* --- run --- */
    syslog(LOG_INFO, "starting FUSE  allow_other=%s",
           allow_other ? "true" : "false");

    int ret = fuse_main(args.argc, args.argv, &emfuse_ops, NULL);

    syslog(LOG_INFO, "emlog_fuse shut down cleanly.");
    fuse_opt_free_args(&args);
    closelog();
    return ret;
}
