
/*
 * EMLOG: the EMbedded-device LOGger
 *
 * Jeremy Elson <jelson@circlemud.org>
 * USC/Information Sciences Institute
 *
 * Modified By:
 * Andreas Neustifter <andreas.neustifter at gmail.com>
 * Andrey Mazo <ahippo at yandex.com>
 * Andriy Stepanov <stanv at altlinux.ru>
 * Darien Kindlund <kindlund at mitre.org>
 * David White <udingmyride at gmail.com>
 * Nicu Pavel <npavel at mini-box.com>
 * Simon Boulet <simon at nostalgeek.com>
 * Thomas Petazzoni <thomas.petazzoni at free-electrons.com>
 *
 * This code is released under the GPL
 *
 * This is emlog version 0.70, released 13 August 2001, modified 25 June 2018
 * For more information see http://www.circlemud.org/~jelson/software/emlog
 * and https://github.com/nicupavel/emlog
 *
 * $Id: emlog.c,v 1.7 2001/08/13 21:29:20 jelson Exp $
 */

/* pr_fmt() taken from infiniband/core/netlink.c */
#define pr_fmt(fmt) "%s:%s: " fmt, KBUILD_MODNAME, __func__
/* to have pr_debug() output in case DYNAMIC_DEBUG is disabled */
#define DEBUG 1

#ifdef MODVERSIONS
#include <linux/modversions.h>
#endif

#include <linux/version.h>
#if LINUX_VERSION_CODE < KERNEL_VERSION(2, 6, 33)
#include <linux/autoconf.h>
#else
#include <generated/autoconf.h>
#endif
#include <linux/stddef.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/workqueue.h>
#include <linux/wait.h>
#include <linux/delay.h>
#include <linux/errno.h>
#include <linux/slab.h>
#include <linux/vmalloc.h>
#include <linux/types.h>
#include <linux/fs.h>
#include <linux/poll.h>
#include <linux/spinlock.h>
#if LINUX_VERSION_CODE < KERNEL_VERSION(4, 11, 0)
#include <linux/sched.h>
#else
#include <linux/sched/signal.h>
#endif

#if LINUX_VERSION_CODE < KERNEL_VERSION(2, 6, 18)
#include <asm/uaccess.h>
#else
#include <linux/uaccess.h>
#endif

/* older kernels don't have pr_{info,err,debug}() at all or lacks pr_fmt() expansion */
#if LINUX_VERSION_CODE < KERNEL_VERSION(2, 6, 28)
#undef pr_err
#undef pr_info
#undef pr_debug
/* copied from 3.14 kernel's printk.h */
#define pr_err(fmt, ...) \
    printk(KERN_ERR pr_fmt(fmt), ##__VA_ARGS__)
#define pr_info(fmt, ...) \
    printk(KERN_INFO pr_fmt(fmt), ##__VA_ARGS__)
#define pr_debug(fmt, ...) \
    printk(KERN_DEBUG pr_fmt(fmt), ##__VA_ARGS__)
#endif

#include "emlog.h"
#include "emlog_ioctl.h"

static bool emlog_autofree = true;
static bool emlog_debug;
static int emlog_max_size = 1024;
static dev_t emlog_dev_type = 0;

static struct cdev *emlog_cdev = NULL;
static struct class *emlog_class = NULL;
static struct device *emlog_dev_reg;
static struct emlog_info *emlog_info_list = NULL;

module_param(emlog_autofree, bool, 0644);
module_param(emlog_debug, bool, 0644);
/* make read-only, so that
 * the number of minor dev numbers to register/unregister doesn't change */
module_param(emlog_max_size, int, 0444);

#define EMLOG_MINOR_BASE    1

/* find the emlog-info structure associated with an inode.  returns a
 * pointer to the structure if found, NULL if not found */
static struct emlog_info *get_einfo(const struct inode *inode)
{
    struct emlog_info *einfo;

    if (inode == NULL)
        return NULL;

    for (einfo = emlog_info_list; einfo != NULL; einfo = einfo->next)
        if (einfo->i_ino == inode->i_ino && einfo->i_rdev == inode->i_rdev)
            return einfo;

    return NULL;
}

/* ========== race condition fixes ========== */
/* to prevent race conditions in the emlog_info_list, we:
 * 1. protect list operations with emlog_list_lock spinlock
 * 2. perform memory allocation outside the lock (vmalloc/vfree may sleep)
 * 3. use double-checked locking pattern in emlog_open
 */
static DEFINE_SPINLOCK(emlog_list_lock);

/* allocate a new emlog_info structure and its buffer, but do not add to list.
 * must be called outside lock since vmalloc may sleep.
 * returns pointer on success, or ERR_PTR on failure. */
static struct emlog_info *alloc_einfo(const struct inode *inode, int minor)
{
    struct emlog_info *einfo;

    if (minor < 1 || minor > emlog_max_size)
        return ERR_PTR(-EINVAL);

    if ((einfo = kzalloc(sizeof(struct emlog_info), GFP_KERNEL)) == NULL)
        return ERR_PTR(-ENOMEM);

    einfo->i_ino = inode->i_ino;
    einfo->i_rdev = inode->i_rdev;

    init_waitqueue_head(EMLOG_READQ(einfo));
    rwlock_init(&einfo->rwlock);

    einfo->size = 1024 * minor;
    if ((einfo->data = vmalloc(sizeof(char) * einfo->size)) == NULL) {
        kfree(einfo);
        return ERR_PTR(-ENOMEM);
    }

    if (emlog_debug)
        pr_debug("allocated einfo for inode %ld.\n", einfo->i_ino);

    return einfo;
}

/* add einfo to the global list.  must be called while holding emlog_list_lock. */
static void insert_einfo(struct emlog_info *einfo)
{
    einfo->next = emlog_info_list;
    emlog_info_list = einfo;
}

/* remove einfo from the global list.  must be called while holding emlog_list_lock. */
static void detach_einfo(struct emlog_info *einfo)
{
    struct emlog_info **ptr;

    ptr = &emlog_info_list;
    while (*ptr != einfo) {
        if (!*ptr) {
            pr_err("corrupt einfo list.\n");
            return;
        }
        ptr = &((**ptr).next);
    }
    *ptr = einfo->next;
}

/* free all memory associated with an emlog_info structure.
 * must be called outside lock since vfree may sleep.
 * don't dereference einfo after calling this! */
static void destroy_einfo(struct emlog_info *einfo)
{
    if (emlog_debug)
        pr_debug("freeing resources associated with inode %ld.\n",
                 einfo->i_ino);
    vfree(einfo->data);
    kfree(einfo);
}

/* drop one reference to einfo.  if it reaches zero and either autofree
 * is set or the buffer is empty, detach it from the list and free it.
 * must be called without emlog_list_lock held.  the caller must already
 * hold a reference to einfo (i.e. einfo->refcount must be >= 1 on entry),
 * so einfo itself cannot have been freed by someone else in the meantime. */
static void put_einfo(struct emlog_info *einfo)
{
    struct emlog_info *to_free = NULL;

    spin_lock(&emlog_list_lock);
    einfo->refcount--;

    /* if no one has this file open and it's not holding any data, or
     * autofree is set, delete the record. */
    if (einfo->refcount == 0) {
        int qlen;
        /* check buffer length under read lock */
        read_lock(&einfo->rwlock);
        qlen = EMLOG_QLEN(einfo);
        read_unlock(&einfo->rwlock);

        if (emlog_autofree || qlen == 0) {
            detach_einfo(einfo);
            to_free = einfo;
        }
    }
    spin_unlock(&emlog_list_lock);

    if (to_free)
        destroy_einfo(to_free);
}

/************************ File Interface Functions ************************/

static int emlog_open(struct inode *inode, struct file *file)
{
    int minor = MINOR(inode->i_rdev);
    struct emlog_info *einfo;
    struct emlog_info *new_einfo = NULL;

    /* look up and take a reference in the same critical section: taking
     * the reference is what keeps einfo alive afterwards, so it must
     * not be dropped and re-acquired in between (that would let a
     * concurrent emlog_release() free einfo out from under us). */
    spin_lock(&emlog_list_lock);
    einfo = get_einfo(inode);
    if (einfo != NULL)
        einfo->refcount++;
    spin_unlock(&emlog_list_lock);

    if (einfo == NULL) {
        /* never heard of this inode before... create a new record.
         * allocate outside lock since vmalloc may sleep. */
        new_einfo = alloc_einfo(inode, minor);
        if (IS_ERR(new_einfo))
            return PTR_ERR(new_einfo);

        spin_lock(&emlog_list_lock);
        /* double-check locking: another thread may have created it while we were allocating */
        einfo = get_einfo(inode);
        if (einfo == NULL) {
            insert_einfo(new_einfo);
            einfo = new_einfo;
            new_einfo = NULL;
        }
        einfo->refcount++;
        spin_unlock(&emlog_list_lock);

        /* if we lost the race, free our unused allocation */
        if (new_einfo != NULL)
            destroy_einfo(new_einfo);
    }

    if (!try_module_get(THIS_MODULE)) {
        pr_err("cannot get module\n");
        put_einfo(einfo);
        return -ENODEV;
    }
    return 0;
}

/* this is called when a file is closed */
static int emlog_release(struct inode *inode, struct file *file)
{
    struct emlog_info *einfo;

    /* this file's own open() holds a reference, so einfo is guaranteed
     * to still be alive here even though we look it up without holding
     * emlog_list_lock continuously. */
    spin_lock(&emlog_list_lock);
    einfo = get_einfo(inode);
    spin_unlock(&emlog_list_lock);

    if (einfo == NULL) {
        pr_err("can not fetch einfo for inode %ld.\n", inode->i_ino);
        module_put(THIS_MODULE);
        return EIO;
    }

    put_einfo(einfo);

    module_put(THIS_MODULE);
    return 0;
}


/* read_from_emlog reads bytes out of a circular buffer with
 * wraparound.  returns char * pointer to data read, which the
 * caller must free.  length is (a pointer to) the number of bytes to
 * be read, which will be set by this function to be the number of
 * bytes actually returned */
static char * read_from_emlog(struct emlog_info * einfo, size_t * length,
                        loff_t * offset)
{
    char *retval;
    int bytes_copied = 0, n, start_point;
    size_t remaining;

    read_lock(&einfo->rwlock);

    if (emlog_debug)
        pr_debug("\nLength: %zu\nOffset: %lld\neinfo->offset: %lld\nEMLOG_FIRST_EMPTY_BYTE: %lld\neinfo->read_point: %zu\neinfo->write_point: %zu\n", *length, *offset, einfo->offset, EMLOG_FIRST_EMPTY_BYTE(einfo), einfo->read_point, einfo->write_point);

    /* is the user trying to read data that has already scrolled off? */
    if (*offset < einfo->offset)
        *offset = einfo->offset;
    if (emlog_debug)
        pr_debug("New Offset: %lld\n", *offset);
    /* is the user trying to read past EOF? */
    if (*offset >= EMLOG_FIRST_EMPTY_BYTE(einfo)) {
        read_unlock(&einfo->rwlock);
        return NULL;
    }

    /* find the smaller of the total bytes we have available and what
     * the user is asking for */
    *length = min_t(size_t, *length, EMLOG_FIRST_EMPTY_BYTE(einfo) - *offset);
    remaining = *length;
    if (emlog_debug)
        pr_debug("Remaining: %zu\n", remaining);

    /* figure out where to start based on user's offset */
    start_point = einfo->read_point + (*offset - einfo->offset);
    if (emlog_debug)
        pr_debug("Start point: %d\n", start_point);
    start_point = start_point % einfo->size;
    if (emlog_debug)
        pr_debug("Start point: %d\n", start_point);

    /* allocate memory to return */
    if ((retval = kmalloc(sizeof(char) * remaining, GFP_KERNEL)) == NULL) {
        read_unlock(&einfo->rwlock);
        return NULL;
    }

    /* copy the (possibly noncontiguous) data to our buffer */
    while (remaining) {
        n = min(remaining, einfo->size - start_point);
        memcpy(retval + bytes_copied, einfo->data + start_point, n);
        bytes_copied += n;
        remaining -= n;
        start_point = (start_point + n) % einfo->size;
    }
    read_unlock(&einfo->rwlock);

    /* advance user's file pointer */
    *offset += *length;
    return retval;
}

static ssize_t emlog_read(struct file *file, char __user *buffer,      /* The buffer to fill with data */
                          size_t length,        /* The length of the buffer */
                          loff_t * offset)
{                               /* Our offset in the file */
    int retval;
    char *data_to_return;
    struct emlog_info *einfo;

    /* get the metadata about this emlog */
    spin_lock(&emlog_list_lock);
    einfo = get_einfo(file->f_path.dentry->d_inode);
    spin_unlock(&emlog_list_lock);
    /* safe to use einfo outside lock: it won't be freed while refcount > 0 */

    if (einfo == NULL) {
        pr_err("can not fetch einfo for inode %ld.\n",
               (long)(file->f_path.dentry->d_inode->i_ino));
        return -EIO;
    }

    /* wait until there's data available (unless we do nonblocking reads) */
    if (file->f_flags & O_NONBLOCK
        && *offset >= EMLOG_FIRST_EMPTY_BYTE(einfo))
        return -EAGAIN;

    wait_event_interruptible((einfo)->read_q,
                             *offset <
                             (einfo)->offset + EMLOG_QLEN(einfo));

    /* see if a signal woke us up */
    if (signal_pending(current))
        return -ERESTARTSYS;

    if ((data_to_return = read_from_emlog(einfo, &length, offset)) == NULL)
        return 0;

    if (copy_to_user(buffer, data_to_return, length) > 0)
        retval = -EFAULT;
    else
        retval = length;
    kfree(data_to_return);
    return retval;
}

/* write_to_emlog writes to a circular buffer with wraparound.  in the
 * case of an overflow, it overwrites the oldest unread data. */
static void write_to_emlog(struct emlog_info *einfo, char *buf, size_t length)
{
    int bytes_copied = 0;
    int overflow = 0;
    int n;

    write_lock(&einfo->rwlock);

    if (emlog_debug)
        pr_debug("\nLength: %zu\nEMLOG_QLEN: %zu\neinfo->size: %zu\neinfo->offset: %lld\neinfo->read_point: %zu\neinfo->write_point: %zu\n", length, EMLOG_QLEN(einfo), einfo->size, einfo->offset, einfo->read_point, einfo->write_point);

    if (length + EMLOG_QLEN(einfo) >= (einfo->size - 1)) {
        overflow = 1;

        /* in case of overflow, figure out where the new buffer will
         * begin.  we start by figuring out where the current buffer ENDS:
         * einfo->offset + EMLOG_QLEN.  we then advance the end-offset
         * by the length of the current write, and work backwards to
         * figure out what the oldest unoverwritten data will be (i.e.,
         * size of the buffer).  was that all quite clear? :-) */
        einfo->offset = einfo->offset + EMLOG_QLEN(einfo) + length
            - einfo->size + 1;
    }

    while (length) {
        /* how many contiguous bytes are available from the write point to
         * the end of the circular buffer? */
        n = min(length, einfo->size - einfo->write_point);
        memcpy(einfo->data + einfo->write_point, buf + bytes_copied, n);
        bytes_copied += n;
        length -= n;
        einfo->write_point = (einfo->write_point + n) % einfo->size;
    }

    /* if there is an overflow, reset the read point to read whatever is
     * the oldest data that we have, that has not yet been
     * overwritten. */
    if (overflow)
        einfo->read_point = (einfo->write_point + 1) % einfo->size;
    write_unlock(&einfo->rwlock);
}

static ssize_t emlog_write(struct file *file,
                           const char __user *buffer,
                           size_t length, loff_t * offset)
{
    char *message = NULL;
    size_t n;
    struct emlog_info *einfo;

    /* get the metadata about this emlog */
    spin_lock(&emlog_list_lock);
    einfo = get_einfo(file->f_path.dentry->d_inode);
    spin_unlock(&emlog_list_lock);

    if (einfo == NULL)
        return -EIO;

    /* if the message is longer than the buffer, just take the beginning
     * of it, in hopes that the reader (if any) will have time to read
     * before we wrap around and obliterate it */
    n = min(length, einfo->size - 1);

    /* make sure we have the memory for it */
    if ((message = kmalloc(n, GFP_KERNEL)) == NULL)
        return -ENOMEM;

    /* copy into our temp buffer */
    if (copy_from_user(message, buffer, n) > 0) {
        kfree(message);
        return -EFAULT;
    }

    /* now copy it into the circular buffer and free our temp copy */
    write_to_emlog(einfo, message, n);
    kfree(message);

    /* wake up any readers that might be waiting for the data.  we call
     * schedule in the vague hope that a reader will run before the
     * writer's next write, to avoid losing data. */
    wake_up_interruptible(EMLOG_READQ(einfo));

    return n;
}

static unsigned int emlog_poll(struct file *file,
                               struct poll_table_struct *wait)
{
    struct emlog_info *einfo;

    /* get the metadata about this emlog */
    spin_lock(&emlog_list_lock);
    einfo = get_einfo(file->f_path.dentry->d_inode);
    spin_unlock(&emlog_list_lock);

    if (einfo == NULL)
        return -EIO;

    poll_wait(file, EMLOG_READQ(einfo), wait);

    if (file->f_pos < EMLOG_FIRST_EMPTY_BYTE(einfo))
        return POLLIN | POLLRDNORM;
    else
        return 0;
}


static long emlog_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
    struct emlog_info *einfo;
    struct emlog_status st;

    /* get the metadata about this emlog */
    spin_lock(&emlog_list_lock);
    einfo = get_einfo(file->f_path.dentry->d_inode);
    spin_unlock(&emlog_list_lock);

    if (einfo == NULL)
        return -EIO;

    switch (cmd) {
    case EMLOG_GET_STATUS:
        /* refcount is protected by emlog_list_lock */
        spin_lock(&emlog_list_lock);
        st.refcount = (__s32)einfo->refcount;
        spin_unlock(&emlog_list_lock);

        /* buffer-related fields are protected by rwlock */
        read_lock(&einfo->rwlock);
        st.buf_size      = (__u32)einfo->size;
        st.data_len      = (__u32)EMLOG_QLEN(einfo);
        st.total_written = (__u64)(einfo->offset + EMLOG_QLEN(einfo));
        read_unlock(&einfo->rwlock);

        if (copy_to_user((void __user *)arg, &st, sizeof(st)))
            return -EFAULT;
        return 0;

    default:
        return -ENOTTY;
    }
}

static const struct file_operations emlog_fops = {
    .read = emlog_read,
    .write = emlog_write,
    .open = emlog_open,
    .release = emlog_release,
    .poll = emlog_poll,
    .unlocked_ioctl  = emlog_ioctl,
#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 12, 0)
    /* no_llseek by default introduced at v2.6.37-rc1 and
     * removed in 6.12.0
     */
    .llseek = no_llseek,
#endif
    .owner = THIS_MODULE,
};

static int __init emlog_init(void)
{
    int ret_val;

    /* Allocate a region of N=emlog_max_size devices.
     * Minor numbers are used to determine the size of the per-device buffer,
     * so allocate one minor number for every possible buffer size (which is <= emlog_max_size).
     */
    const int emlog_minor_count = emlog_max_size;
    ret_val = alloc_chrdev_region(&emlog_dev_type, EMLOG_MINOR_BASE, emlog_minor_count, DEVICE_NAME);
    if (ret_val < 0) {
        pr_err("Can not alloc_chrdev_region, error code %d.\n", ret_val);
        return -1;
    }

    emlog_cdev = cdev_alloc();
    if (emlog_cdev == NULL) {
        pr_err("Can not cdev_alloc.\n");
        ret_val = -2; goto emlog_init_error;
    }

    emlog_cdev->ops = &emlog_fops;
    emlog_cdev->owner = THIS_MODULE;

    ret_val = cdev_add(emlog_cdev, emlog_dev_type, emlog_minor_count);
    if (ret_val < 0) {
        pr_err("Can not cdev_add, error code %d.\n", ret_val);
        ret_val = -3; goto emlog_init_error;
    }

    pr_info("version %s running, major is %u, MINOR is %u, max size %d K.\n", EMLOG_VERSION, (unsigned)MAJOR(emlog_dev_type), (unsigned)MINOR(emlog_dev_type), emlog_max_size);

#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 4, 0)
    emlog_class = class_create(THIS_MODULE, DEVICE_NAME);
#else
    emlog_class = class_create(DEVICE_NAME);
#endif
    if (emlog_class == NULL) {
        pr_err("Can not class_create.\n");
        ret_val = -4; goto emlog_init_error;
    }

    emlog_dev_reg = device_create(emlog_class, NULL, MKDEV(MAJOR(emlog_dev_type), 256), NULL, DEVICE_NAME );
    if (emlog_dev_reg == NULL) {
        pr_err("Can not device_create.\n");
        ret_val = -5; goto emlog_init_error;
    }

    goto emlog_init_okay;
  emlog_init_error:
    if (emlog_dev_reg) device_destroy(emlog_class, emlog_dev_type);
    if (emlog_class) class_destroy(emlog_class);
    if (emlog_cdev) cdev_del(emlog_cdev);
    if (emlog_dev_type) unregister_chrdev_region(emlog_dev_type, emlog_minor_count);
  emlog_init_okay:
    return ret_val;
}

static void __exit emlog_remove(void)
{
    struct emlog_info *to_free;

    /* clean up any still-allocated einfo structures.
     * lock/unlock around each destroy to avoid sleeping in spinlock. */
    spin_lock(&emlog_list_lock);
    while (emlog_info_list != NULL) {
        to_free = emlog_info_list;
        detach_einfo(to_free);
        spin_unlock(&emlog_list_lock);
        destroy_einfo(to_free);
        spin_lock(&emlog_list_lock);
    }
    spin_unlock(&emlog_list_lock);

    device_destroy(emlog_class, emlog_dev_type);
    class_destroy(emlog_class);
    cdev_del(emlog_cdev);
    unregister_chrdev_region(emlog_dev_type, emlog_max_size /* aka emlog_minor_count */);

    pr_info("unloaded.\n");
}

module_init(emlog_init);
module_exit(emlog_remove);

MODULE_LICENSE("GPL v2");
