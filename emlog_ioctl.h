/*
 * emlog_ioctl.h - ioctl definitions for emlog buffer status query
 *
 * Include from both kernel module and userspace programs.
 */

#ifndef EMLOG_IOCTL_H
#define EMLOG_IOCTL_H

#ifdef __KERNEL__
#include <linux/ioctl.h>
#include <linux/types.h>
#else
#include <sys/ioctl.h>
#include <stdint.h>
typedef uint32_t __u32;
typedef uint64_t __u64;
typedef int32_t  __s32;
#endif

#define EMLOG_IOCTL_MAGIC  'E'

struct emlog_status {
    __u32 buf_size;      /* total buffer size in bytes */
    __u32 data_len;      /* current data length in bytes */
    __u64 total_written; /* total bytes ever written */
    __s32 refcount;      /* number of open file descriptors */
};

#define EMLOG_GET_STATUS  _IOR(EMLOG_IOCTL_MAGIC, 1, struct emlog_status)

#endif /* EMLOG_IOCTL_H */
