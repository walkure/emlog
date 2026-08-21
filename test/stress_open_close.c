/*
 * stress_open_close.c - repeatedly open+write+close a device path as fast
 * as possible.  Used by run_kernel_race_test.sh, launched as many parallel
 * copies, to hammer the emlog_open()/emlog_release() refcount path: this
 * is a regression test for the emlog_open() use-after-free that used to
 * happen when one thread looked up an existing einfo, dropped the list
 * lock, and only then incremented its refcount -- racing against another
 * thread's emlog_release() freeing that einfo in between (autofree
 * defaults to true, so every close-to-zero is a free).
 *
 * usage: stress_open_close <path> <iterations>
 * exits 0 on success, 1 on the first open/write failure.
 */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int main(int argc, char **argv)
{
    if (argc != 3) {
        fprintf(stderr, "usage: %s <path> <iterations>\n", argv[0]);
        return 2;
    }

    const char *path = argv[1];
    long iters = atol(argv[2]);
    char buf[32];

    for (long i = 0; i < iters; i++) {
        int fd = open(path, O_RDWR);
        if (fd < 0) {
            fprintf(stderr, "[pid %d] open failed at iter %ld: %s\n",
                    getpid(), i, strerror(errno));
            return 1;
        }

        int n = snprintf(buf, sizeof(buf), "%d-%ld\n", getpid(), i);
        if (write(fd, buf, (size_t)n) < 0) {
            fprintf(stderr, "[pid %d] write failed at iter %ld: %s\n",
                    getpid(), i, strerror(errno));
            close(fd);
            return 1;
        }

        close(fd);
    }

    return 0;
}
