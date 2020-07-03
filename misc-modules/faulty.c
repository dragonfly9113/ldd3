/*
 * faulty.c -- a module which generates an oops when read or write
 *
 * compiled and tested on 4.15.0 kernel
 */

#include <linux/module.h>
#include <linux/init.h>

#include <linux/kernel.h>
#include <linux/fs.h>
#include <linux/types.h>
#include <linux/uaccess.h>

MODULE_LICENSE("Dual BSD/GPL");


int faulty_major = 0;

ssize_t faulty_read(struct file *filp, char __user *buf,
		size_t count, loff_t *pos)
{
	int ret;
	char stack_buf[4];

	/* Let's try a buffer overflow: cannot do this any more because compiler will not pass */ 
	memset(stack_buf, 0xff, 4);

	if (count > 4)
		count = 4;	/* copy 4 bytes to the user */
	ret = copy_to_user(buf, stack_buf, count);
	if (!ret)
		return count;
	return ret;
}

ssize_t faulty_write(struct file *flip, const char __user *buf, size_t count,
		loff_t *pos)
{
	/* make a simple fault by dereferencing a NULL pointer */
	*(int *)0 = 0;
	return 0;
}

struct file_operations faulty_fops = {
	.read = faulty_read,
	.write = faulty_write,
	.owner = THIS_MODULE
};

int faulty_init(void)
{
	int result;

	/*
	 * Register your major, and accept a dynamic number
	 */
	result = register_chrdev(faulty_major, "faulty", &faulty_fops);
	if (result < 0)
		return result;
	if (faulty_major == 0)
		faulty_major = result;	/* dynamic */

	return 0;
}

void faulty_cleanup(void)
{
	unregister_chrdev(faulty_major, "faulty");
}

module_init(faulty_init);
module_exit(faulty_cleanup);


