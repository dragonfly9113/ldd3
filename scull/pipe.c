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

















