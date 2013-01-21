/*
 * RAM Oops/Panic logger
 *
 * Copyright © 2012 Pure Storage, Inc.
 *
 * This program is free software; you may redistribute it and/or
 * modify it under the terms of the GNU General Public License version
 * 2 as published by the Free Software Foundation.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
 * BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
 * ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/err.h>
#include <linux/fs.h>
#include <linux/kernel.h>
#include <linux/kmsg_dump.h>
#include <linux/miscdevice.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/time.h>
#include <linux/vmalloc.h>

#define BUFOOPS_DEFAULT_REC_SIZE	(1 << 18)
#define BUFOOPS_DEFAULT_NUM_REC		8

static unsigned int rec_size = BUFOOPS_DEFAULT_REC_SIZE;
module_param(rec_size, uint, S_IRUGO);
MODULE_PARM_DESC(rec_size, "size of each oops/panic dump record");

static unsigned int num_rec = BUFOOPS_DEFAULT_NUM_REC;
module_param(num_rec, uint, S_IRUGO);
MODULE_PARM_DESC(num_rec, "number of dump records in buffer");

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Roland Dreier <roland@purestorage.com>");
MODULE_DESCRIPTION("Mappable buffer oops/panic logger/driver");

struct bufoops_header {
	u64     hdr_size;
	u64	rec_size;
	u64	num_rec;
};

static unsigned long dump_buf_size;
static unsigned long buf_size;

static void    *buf;
static void    *dump_buf;
static int	pos;

/* XXX need locking for simultaneous dumps? */
static void bufoops_do_dump(struct kmsg_dumper *dumper,
			    enum kmsg_dump_reason reason,
			    const char *s1, unsigned long l1,
			    const char *s2, unsigned long l2)
{
	void *cur_buf;
	unsigned long off;
	unsigned long s1_start, s2_start;
	unsigned long l1_cpy, l2_cpy;
	struct timeval timestamp;

	if (reason != KMSG_DUMP_OOPS &&
	    reason != KMSG_DUMP_PANIC)
		return;

	cur_buf = dump_buf + pos;

	memset(cur_buf, 0, rec_size);

	do_gettimeofday(&timestamp);
	off = sprintf(cur_buf, "\n====> %9lu.%06lu [dump reason %d]\n\n",
		      (long) timestamp.tv_sec, (long) timestamp.tv_usec, reason);
	cur_buf += off;

	l2_cpy = min(l2, rec_size - off - 1);
	l1_cpy = min(l1, rec_size - off - l2_cpy - 1);

	s2_start = l2 - l2_cpy;
	s1_start = l1 - l1_cpy;

	memcpy(cur_buf, s1 + s1_start, l1_cpy);
	memcpy(cur_buf + l1_cpy, s2 + s2_start, l2_cpy);

	pos += rec_size;
	if (pos >= dump_buf_size)
		pos = 0;

	*((char *) dump_buf + pos) = 0;
}

static struct kmsg_dumper bufoops_dumper = {
	.dump = bufoops_do_dump,
};

static int bufoops_fault(struct vm_area_struct *vma, struct vm_fault *vmf)
{
	vmf->page = vmalloc_to_page(buf + (vmf->pgoff << PAGE_SHIFT));
	get_page(vmf->page);

	return 0;
}

static const struct vm_operations_struct bufoops_vm_ops = {
	.fault		= bufoops_fault
};

static int bufoops_mmap(struct file *filp, struct vm_area_struct *vma)
{
	/* Don't allow mapping anything past the end of our buffer. */
	if ((vma->vm_pgoff << PAGE_SHIFT) + (vma->vm_end - vma->vm_start) > buf_size)
		return -EINVAL;

	vma->vm_ops = &bufoops_vm_ops;
	return 0;
}

static const struct file_operations bufoops_fops = {
	.owner	= THIS_MODULE,
	.open	= nonseekable_open,
	.mmap	= bufoops_mmap,
	.llseek	= no_llseek,
};

static struct miscdevice bufoops_misc = {
	.minor	= MISC_DYNAMIC_MINOR,
	.name	= "bufoops",
	.fops	= &bufoops_fops,
	.mode	= S_IRUSR,
};

static void bufoops_init_header(struct bufoops_header *hdr)
{
	hdr->hdr_size = PAGE_SIZE;
	hdr->rec_size = rec_size;
	hdr->num_rec  = num_rec;
}

static int __init bufoops_init(void)
{
	int ret = -ENOMEM;

	rec_size = rec_size ? rounddown_pow_of_two(rec_size) : BUFOOPS_DEFAULT_REC_SIZE;
	num_rec  = num_rec  ? rounddown_pow_of_two(num_rec)  : BUFOOPS_DEFAULT_NUM_REC;

	if (num_rec > ULONG_MAX / rec_size) {
		pr_err("rec_size %u * num_rec %u overflows.\n", rec_size, num_rec);
		goto err;
	}

	dump_buf_size = (unsigned long) num_rec * (unsigned long) rec_size;
	if (dump_buf_size > ULONG_MAX - PAGE_SIZE) {
		pr_err("rec_size %u * num_rec %u overflows.\n", rec_size, num_rec);
		goto err;
	}

	buf_size = dump_buf_size + PAGE_SIZE;

	buf = vmalloc_user(buf_size);
	if (!buf)
		goto err;

	dump_buf = buf + PAGE_SIZE;
	bufoops_init_header(buf);

	ret = misc_register(&bufoops_misc);
	if (ret) {
		pr_warning("misc_register failed: %d\n", ret);
		goto err_buf;
	}

	ret = kmsg_dump_register(&bufoops_dumper);
	if (ret) {
		pr_warning("kmsg_dump_register failed: %d\n", ret);
		goto err_miscdev;
	}

	return 0;

err_miscdev:
	misc_deregister(&bufoops_misc);

err_buf:
	vfree(buf);

err:
	return ret;
}

static void __exit bufoops_exit(void)
{
	int ret = kmsg_dump_unregister(&bufoops_dumper);
	if (ret)
		pr_warning("kmsg_dump_unregister failed: %d\n", ret);

	misc_deregister(&bufoops_misc);
	vfree(buf);
}

module_init(bufoops_init);
module_exit(bufoops_exit);
