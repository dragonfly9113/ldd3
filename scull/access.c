/*
 * access.c -- the files with access control on open
 *
 * compiled and tested on kernel 4.15.0
 *
 */

/* FIXME: cloned devices as a use for kobjects? */

#include <linux/kernel.h>	/* printk() */
#include <linux/module.h>
#include <linux/slab.h>		/* kmalloc() */
#include <linux/fs.h>		/* everything */
#include <linux/errno.h>	/* error codes */
#include <linux/types.h>	/* size_t */
#include <linux/fcntl.h>
#include <linux/cdev.h>
#include <linux/tty.h>
#include <linux/atomic.h>
#include <linux/list.h>

#include "scull.h"		/* local definitions */

static dev_t scull_a_firstdev;	/* where our range begins */

/* 
 * These devices fall back on the main scull operations. They only
 * differ in the implementations of open() and close()
 */


/************************************************************************
 * The first device is the single-open one,
 * it has an hw structure and an open count
 */

static struct scull_dev scull_s_device;
static atomic_t scull_s_available = ATOMIC_INIT(1);

static int scull_s_open(struct inode *inode, struct file *filp)
{
	struct scull_dev *dev = &scull_s_device;	/* device information */

	PDEBUG("scull_s_open() is called, scull_s_available = %i\n", atomic_read(&scull_s_available));

	if (! atomic_dec_and_test(&scull_s_available)) {
		atomic_inc(&scull_s_available);
		return -EBUSY;	/* already open */
	}

	PDEBUG("After atomic_dec_and_test(), scull_s_available = %i\n", atomic_read(&scull_s_available));

	/* then, everything else is copied from the base scull device */
	if ((filp->f_flags & O_ACCMODE) == O_WRONLY)
		scull_trim(dev);
	filp->private_data = dev;
	return 0;	/* success */
}

static int scull_s_release(struct inode *inode, struct file *filp)
{
	atomic_inc(&scull_s_available);	/* release the device */

	PDEBUG("scull_s_release() is called, scull_s_available = %i\n", atomic_read(&scull_s_available));

	return 0;
}

/*
 * The other operations for the single-open device come from the bare device
 */
struct file_operations scull_sngl_fops = {
	.owner = 	THIS_MODULE,
	.llseek = 	scull_llseek,
	.read =		scull_read,
	.write = 	scull_write,
	.compat_ioctl =	scull_ioctl,
	.open =		scull_s_open,
	.release = 	scull_s_release,
};


/********************************************************************************
 *
 * And the init and cleanup functions come last
 */

static struct scull_adev_info {
	char *name;
	struct scull_dev *sculldev;
	struct file_operations *fops;
} scull_access_devs[] = {
	{ "scullsingle", &scull_s_device, &scull_sngl_fops },
};

#define SCULL_N_ADEVS 1

/*
 * Set up a single device.
 */
static void scull_access_setup (dev_t devno, struct scull_adev_info *devinfo)
{
	struct scull_dev *dev = devinfo->sculldev;
	int err;

	/* Initialize the device structure */
	dev->quantum = scull_quantum;
	dev->qset = scull_qset;
	sema_init(&dev->sem, 1);

	/* Do the cdev stuff */
	cdev_init(&dev->cdev, devinfo->fops);
	kobject_set_name(&dev->cdev.kobj, devinfo->name);
	dev->cdev.owner = THIS_MODULE;
	err = cdev_add(&dev->cdev, devno, 1);
	/* Fail gracefully if need be */
	if (err) {
		printk(KERN_NOTICE "Error %d adding %s\n", err, devinfo->name);
		kobject_put(&dev->cdev.kobj);
	} else
		printk(KERN_NOTICE "%s registered at %x\n", devinfo->name, devno);
}

int scull_access_init(dev_t firstdev)
{
	int result, i;

	/* Get our number space */
	result = register_chrdev_region(firstdev, SCULL_N_ADEVS, "sculla");
	if (result < 0) {
		printk(KERN_WARNING "sculla: device number registration failed\n");
		return 0;
	}
	scull_a_firstdev = firstdev;

	/* Set up each device */
	for (i = 0; i < SCULL_N_ADEVS; i++)
		scull_access_setup(firstdev + i, scull_access_devs + i);
	return SCULL_N_ADEVS;
}

/*
 * This is called by cleanup_module or on failire.
 * It is required to never fail, even if nothing was initialized first
 */
void scull_access_cleanup(void)
{
	//struct scull_listitem *lptr, *next;
	int i;

	/* Clean up the static devs */
	for (i = 0; i < SCULL_N_ADEVS; i++) {
		struct scull_dev *dev = scull_access_devs[i].sculldev;
		cdev_del(&dev->cdev);
		scull_trim(scull_access_devs[i].sculldev);
	}

	/* TODO And all the cloned devices */


	/* Free up our number space */
	unregister_chrdev_region(scull_a_firstdev, SCULL_N_ADEVS);
	return;
}


