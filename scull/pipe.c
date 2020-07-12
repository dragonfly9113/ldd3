/*
 * pipe.c -- fifo driver for scull
 *
 * compiled and tested on kernel 4.15.0
 *
 */

#include <linux/module.h>
#include <linux/moduleparam.h>

#include <linux/kernel.h>	/* printk(), min() */
#include <linux/slab.h>		/* kmalloc() */
#include <linux/fs.h>		/* everyting... */
#include <linux/proc_fs.h>
#include <linux/errno.h>	/* error codes */
#include <linux/types.h>	/* size_t */
#include <linux/fcntl.h>
#include <linux/poll.h>		/* includes wait.h */
#include <linux/cdev.h>
#include <linux/uaccess.h>
#include <linux/sched/signal.h>

#include "scull.h"		/* local definitions */

struct scull_pipe {
	wait_queue_head_t inq, outq;		/* read and write queues */
	char *buffer, *end;			/* begin of buf, end of buf */
	int buffersize;				/* used in pointer arithmetic */
	char *rp, *wp;				/* where to read, where to write */
	int nreaders, nwriters;			/* number of openings for r/w */
	struct fasync_struct *async_queue;	/* asynchronous readers */
	struct semaphore sem;			/* mutual exclusion semaphore */
	struct cdev cdev;			/* Char device structure */
};

/* parameters */
static int scull_p_nr_devs = SCULL_P_NR_DEVS;	/* number of pipe devices */
int scull_p_buffer = SCULL_P_BUFFER;	/* buffer size */
dev_t scull_p_devno;			/* Our first device number */

module_param(scull_p_nr_devs, int, 0);
module_param(scull_p_buffer, int, 0);

static struct scull_pipe *scull_p_devices;

static int scull_p_fasync(int fd, struct file *filp, int mode);
static int spacefree(struct scull_pipe *dev);

/*
 * Open and close
 */

static int scull_p_open(struct inode *inode, struct file *filp)
{
	struct scull_pipe *dev;

	dev = container_of(inode->i_cdev, struct scull_pipe, cdev);
	filp->private_data = dev;

	PDEBUG("scull_p_open() is called\n");

	if (down_interruptible(&dev->sem))
		return -ERESTARTSYS;
	if (!dev->buffer) {
		/* allocate the buffer */
		dev->buffer = kmalloc(scull_p_buffer, GFP_KERNEL);
		if (!dev->buffer) {
			up(&dev->sem);
			return -ENOMEM;
		}

		dev->buffersize = scull_p_buffer;
		dev->end = dev->buffer + dev->buffersize;
		dev->rp = dev->wp = dev->buffer;	/* rd and wr from the beginning */
	}
	//dev->buffersize = scull_p_buffer;
	//dev->end = dev->buffer + dev->buffersize;
	//dev->rp = dev->wp = dev->buffer;	/* rd and wr from the beginning */

	/* use f_mode, not f_flags: it's cleaner (fs/open.c tells why) */
	if (filp->f_mode & FMODE_READ)
		dev->nreaders++;
	if (filp->f_mode & FMODE_WRITE)
		dev->nwriters++;
	up(&dev->sem);

	return nonseekable_open(inode, filp);
}

static int scull_p_release(struct inode *inode, struct file *filp)
{
	struct scull_pipe *dev = filp->private_data;

	PDEBUG("scull_p_release() is called\n");

	/* remove this filp from the asynchronously notified filp's */
	scull_p_fasync(-1, filp, 0);
	down(&dev->sem);
	if (filp->f_mode & FMODE_READ)
		dev->nreaders--;
	if (filp->f_mode & FMODE_WRITE)
		dev->nwriters--;
	if (dev->nreaders + dev->nwriters == 0) {
		kfree(dev->buffer);
		dev->buffer = NULL;	/* the other fields are not checked on open */
	}
	up(&dev->sem);
	return 0;
}

/*
 * Data management: read and write
 */

static ssize_t scull_p_read (struct file *filp, char __user *buf, size_t count,
		loff_t *pos)
{
	struct scull_pipe *dev = filp->private_data;

	if (down_interruptible(&dev->sem))
		return -ERESTARTSYS;

	while (dev->rp == dev->wp) {	/* nothing to read */
		up(&dev->sem);	/* release the lock */
		if (filp->f_flags & O_NONBLOCK)
			return -EAGAIN;
		PDEBUG("\"%s\" reading: going to sleep\n", current->comm);
		if (wait_event_interruptible(dev->inq, (dev->rp != dev->wp)))
			return -ERESTARTSYS;	/* signal: tell the fs layer to handle it */
		/* otherwise loop, but first reacquire the lock */
		if (down_interruptible(&dev->sem))
			return ERESTARTSYS;
	}
	/* ok, data is there, return something */
	if (dev->wp > dev->rp)
		count = min(count, (size_t)(dev->wp - dev->rp));
	else	/* the writer point has wrapped, return data up to dev->end */
		count = min(count, (size_t)(dev->end - dev->rp));
	if (copy_to_user(buf, dev->rp, count)) {
		up(&dev->sem);
		return -EFAULT;
	}
	dev->rp += count;
	if (dev->rp == dev->end)	
		dev->rp = dev->buffer;	/* wrapped */
	up (&dev->sem);

	/* finally, awake any writers and return */
	wake_up_interruptible(&dev->outq);
	PDEBUG("\"%s\" did read %li bytes\n", current->comm, (long)count);
	return count;
}

/* Wait for space for writing; caller must hold device semaphore. On
 * error the semaphore will be released before returning. */
static int scull_getwritespace(struct scull_pipe *dev, struct file *filp)
{
	while (spacefree(dev) == 0) {	/* full */
		DEFINE_WAIT(wait);

		up(&dev->sem);
		if (filp->f_flags & O_NONBLOCK)
			return -EAGAIN;	
		PDEBUG("\"%s\" writing: going to sleep\n", current->comm);
		prepare_to_wait(&dev->outq, &wait, TASK_INTERRUPTIBLE);
		if (spacefree(dev) == 0)
			schedule();
		finish_wait(&dev->outq, &wait);
		if (signal_pending(current))
			return -ERESTARTSYS;
		if (down_interruptible(&dev->sem))
			return -ERESTARTSYS;
	}
	return 0;
}

/* How much space is free? */
static int spacefree(struct scull_pipe *dev)
{
	if (dev->rp == dev->wp)
		return dev->buffersize - 1;
	return ((dev->rp + dev->buffersize - dev->wp) % dev->buffersize) - 1;
}

static ssize_t scull_p_write(struct file *filp, const char __user *buf, size_t count,
		loff_t *f_pos)
{
	struct scull_pipe *dev = filp->private_data;
	int result;

	if (down_interruptible(&dev->sem))
		return -ERESTARTSYS;

	/* Make sure there's space to write */
	result = scull_getwritespace(dev, filp);
	if (result)
		return result;	/* scull_getwritespace called up(&dev->sem) */

	/* ok, space is there, accept something */
	count = min(count, (size_t)spacefree(dev));
	if (dev->wp >= dev->rp)
		count = min(count, (size_t)(dev->end - dev->wp));	/* to end of buf */
	else	/* the write pointer has wrapped, fill up to rp - 1 */
		count = min(count, (size_t)(dev->rp - dev->wp - 1));
	PDEBUG("Going to accept %li bytes to %p from %p\n", (long)count, dev->wp, buf);
	if (copy_from_user(dev->wp, buf, count)) {
		up (&dev->sem);
		return -EFAULT;
	}
	dev->wp += count;
	if (dev->wp == dev->end)
		dev->wp = dev->buffer;	/* wrapped */
	up(&dev->sem);

	/* finally, awake any reader */
	wake_up_interruptible(&dev->inq);	/* blocked in read() and select() */

	/* and signal asynchronous readers, explained late in chapter 5 */
	if (dev->async_queue)
		kill_fasync(&dev->async_queue, SIGIO, POLL_IN);
	PDEBUG("\"%s\" did write %li bytes\n", current->comm, (long)count);
	return count;
}

static unsigned int scull_p_poll(struct file *filp, poll_table *wait)
{
	return 0;
}

static int scull_p_fasync(int fd, struct file *filp, int mode)
{
	return 0;
}

#ifdef SCULL_DEBUG

/*
 * Here are our sequence iteration methods. Our "position" is
 * simply the device number.
 */
static void *scull_p_seq_start(struct seq_file *s, loff_t *pos)
{
	if (*pos >= scull_p_nr_devs)
		return NULL;	/* No more to read */
	return scull_p_devices + *pos;
}

static void *scull_p_seq_next(struct seq_file *s, void *v, loff_t *pos)
{
	(*pos)++;
	if (*pos >= scull_p_nr_devs)	
		return NULL;
	return scull_p_devices + *pos;
}

static void scull_p_seq_stop(struct seq_file *s, void *v)
{
	/* Actually, there is nothing to do here */
}

static int scull_p_seq_show(struct seq_file *s, void *v)
{
	struct scull_pipe *p = (struct scull_pipe *) v;
	
	if (down_interruptible(&p->sem))
		return -ERESTARTSYS;

	seq_printf(s, "Default buffersize is %i, scull_p_devices = %p\n", scull_p_buffer, scull_p_devices);
	seq_printf(s, "\nDevice %i: %p\n", (int)(p - scull_p_devices), p);
	seq_printf(s, "   Buffer: %p to %p (%i bytes)\n", p->buffer, p->end, p->buffersize);
	seq_printf(s, "   rp %p   wp %p		wp-rp= %li\n", p->rp, p->wp, p->wp - p->rp);
	seq_printf(s, "   readers %i   writers %i\n", p->nreaders, p->nwriters);

	up(&p->sem);
	return 0;
}

/*
 * Tie the sequence operations up.
 */
static struct seq_operations scull_p_seq_ops = {
	.start = scull_p_seq_start,
	.next  = scull_p_seq_next,
	.stop  = scull_p_seq_stop,
	.show  = scull_p_seq_show
};

/*
 * Now to implement the /proc file we need only make an open
 * method which sets up the sequence operators.
 */
static int scull_p_proc_open(struct inode *inode, struct file *file)
{
	return seq_open(file, &scull_p_seq_ops);
}

/*
 * Create a set of file operations for our proc file.
 */
static struct file_operations scull_p_proc_ops = {
	.owner	 = THIS_MODULE,
	.open	 = scull_p_proc_open,
	.read	 = seq_read,
	.llseek	 = seq_lseek,
	.release = seq_release
};

/*
 * Actually create (and remove) the /proc file(s).
 */
void scull_p_create_proc(void)
{
	struct proc_dir_entry *entry;

	entry = proc_create("scullpseq", 0, NULL, &scull_p_proc_ops);
}

void scull_p_remove_proc(void)
{
	/* no problem if it was not registered */
	remove_proc_entry("scullpseq", NULL);
}


#endif	/* SCULL_DEBUG */


/*
 * The file operations for the pipe device
 * (some are overlayered with bare scull)
 */
struct file_operations scull_pipe_fops = {
	.owner = 	THIS_MODULE,
	.llseek =	no_llseek,
	.read =		scull_p_read,
	.write = 	scull_p_write,
	.poll = 	scull_p_poll,
	.compat_ioctl =	scull_ioctl,
	.open = 	scull_p_open,
	.release = 	scull_p_release,
	.fasync = 	scull_p_fasync,
};

/*
 * Set up a cdev entry
 */
static void scull_p_setup_cdev(struct scull_pipe *dev, int index)
{
	int err, devno = scull_p_devno + index;

	cdev_init(&dev->cdev, &scull_pipe_fops);
	dev->cdev.owner = THIS_MODULE;
	err = cdev_add (&dev->cdev, devno, 1);
	/* Fail gracefully if need be */
	if (err)
		printk(KERN_NOTICE "Error %d adding scullpipe%d\n", err, index);
}

/*
 * Initialize the pipe devs; return how many we did.
 */
int scull_p_init(dev_t firstdev)
{
	int i, result;

	PDEBUG("scull_p_init() is called, firstdev = %i\n", firstdev);

	result = register_chrdev_region(firstdev, scull_p_nr_devs, "scullp");
	if (result < 0) {
		printk(KERN_NOTICE "Unable to get scullp region, error %d\n", result);
		return 0;
	}
	scull_p_devno = firstdev;
	scull_p_devices = kmalloc(scull_p_nr_devs * sizeof(struct scull_pipe), GFP_KERNEL);
	if (scull_p_devices == NULL) {
		unregister_chrdev_region(firstdev, scull_p_nr_devs);
		return 0;
	}
	memset(scull_p_devices, 0, scull_p_nr_devs * sizeof(struct scull_pipe));
	for (i = 0; i < scull_p_nr_devs; i++) {
		init_waitqueue_head(&(scull_p_devices[i].inq));		
		init_waitqueue_head(&(scull_p_devices[i].outq));		
		sema_init(&scull_p_devices[i].sem, 1);
		scull_p_setup_cdev(scull_p_devices + i, i);
	}
#ifdef SCULL_DEBUG
	/* obsolete */
	//create_proc_read_entry("scullpipe", 0, NULL, scull_read_p_mem, NULL);
#endif
	return scull_p_nr_devs;
}

/*
 * This is called by cleanup_module or on failure.
 * It is required to never fail, even if nothing was initialized first
 */
void scull_p_cleanup(void)
{
	int i;

#ifdef SCULL_DEBUG
	/* obsolete */
	//remove_proc_entry("scullpipe", NULL);
#endif
	PDEBUG("scull_p_cleanup() is called\n");

	if (!scull_p_devices)
		return;	/* nothing else to release */

	for (i = 0; i < scull_p_nr_devs; i++) {
		cdev_del(&scull_p_devices[i].cdev);
		kfree(scull_p_devices[i].buffer);
	}
	kfree(scull_p_devices);	
	unregister_chrdev_region(scull_p_devno, scull_p_nr_devs);
	scull_p_devices = NULL;	/* pedantic */
}


