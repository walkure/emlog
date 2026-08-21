/*
 * emlog_stat - query emlog buffer status via ioctl
 *
 * Build: gcc -Wall -o emlog_stat emlog_stat.c
 * Usage: emlog_stat /some/emlog/device
 */

#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include "emlog_ioctl.h"

int main(int argc, char *argv[])
{
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <emlog-device> [...]\n", argv[0]);
        return 1;
    }

    int had_error = 0;

    for (int i = 1; i < argc; i++) {
        int fd = open(argv[i], O_RDONLY | O_NONBLOCK);
        if (fd < 0) {
            perror(argv[i]);
            had_error = 1;
            continue;
        }

        struct emlog_status st;
        if (ioctl(fd, EMLOG_GET_STATUS, &st) < 0) {
            perror("ioctl EMLOG_GET_STATUS");
            close(fd);
            had_error = 1;
            continue;
        }
        close(fd);

        printf("%s:\n", argv[i]);
        printf("  buffer size   : %u bytes (%u KB)\n", st.buf_size, st.buf_size / 1024);
        printf("  data length   : %u bytes (%.1f%%)\n", st.data_len,
               st.buf_size ? 100.0 * st.data_len / st.buf_size : 0);
        printf("  total written : %llu bytes\n", (unsigned long long)st.total_written);
        printf("  refcount      : %d\n", st.refcount);
    }
    return had_error ? 1 : 0;
}
