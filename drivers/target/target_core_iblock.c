/*******************************************************************************
 * Filename:  target_core_iblock.c
 *
 * This file contains the Storage Engine  <-> Linux BlockIO transport
 * specific functions.
 *
 * Copyright (c) 2003, 2004, 2005 PyX Technologies, Inc.
 * Copyright (c) 2005, 2006, 2007 SBE, Inc.
 * Copyright (c) 2007-2010 Rising Tide Systems
 * Copyright (c) 2008-2010 Linux-iSCSI.org
 *
 * Nicholas A. Bellinger <nab@kernel.org>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
 *
 ******************************************************************************/

#define pr_fmt(fmt) "%s:%s: " fmt, KBUILD_MODNAME, __func__

#include <linux/string.h>
#include <linux/parser.h>
#include <linux/timer.h>
#include <linux/fs.h>
#include <linux/blkdev.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/bio.h>
#include <linux/genhd.h>
#include <linux/file.h>
#include <linux/module.h>
#include <asm/unaligned.h>
#include <scsi/scsi.h>
#include <scsi/scsi_host.h>

#include <target/target_core_base.h>
#include <target/target_core_backend.h>
#include <target/target_core_fabric.h>

#include "target_core_iblock.h"

#define IBLOCK_MAX_BIO_PER_TASK	 32	/* max # of bios to submit at a time */
#define IBLOCK_BIO_POOL_SIZE	128

static char pure_product_id[17] = PURE_PRODUCT_ID;
static char pure_revision[5]    = PURE_REVISION;

module_param_string(pure_pid, pure_product_id, sizeof(pure_product_id), 0644);
MODULE_PARM_DESC(pure_pid, "PURE Product identification");

module_param_string(pure_rev, pure_revision, sizeof(pure_revision), 0644);
MODULE_PARM_DESC(pure_rev, "PURE Product revision");

static struct se_subsystem_api iblock_template;

static void iblock_bio_done(struct bio *, int);
static void iblock_caw_bio_done(struct bio *, int);

/*	iblock_attach_hba(): (Part of se_subsystem_api_t template)
 *
 *
 */
static int iblock_attach_hba(struct se_hba *hba, u32 host_id)
{
	pr_debug("CORE_HBA[%d] - TCM iBlock HBA Driver %s on"
		" Generic Target Core Stack %s\n", hba->hba_id,
		IBLOCK_VERSION, TARGET_CORE_MOD_VERSION);
	return 0;
}

static void iblock_detach_hba(struct se_hba *hba)
{
}

static void *iblock_allocate_virtdevice(struct se_hba *hba, const char *name)
{
	struct iblock_dev *ib_dev = NULL;

	ib_dev = kzalloc(sizeof(struct iblock_dev), GFP_KERNEL);
	if (!ib_dev) {
		pr_err("Unable to allocate struct iblock_dev\n");
		return NULL;
	}

	pr_debug( "IBLOCK: Allocated ib_dev for %s\n", name);

	return ib_dev;
}

static struct se_device *iblock_create_virtdevice(
	struct se_hba *hba,
	struct se_subsystem_dev *se_dev,
	void *p)
{
	struct iblock_dev *ib_dev = p;
	struct se_device *dev;
	struct se_dev_limits dev_limits;
	struct block_device *bd = NULL;
	struct request_queue *q;
	struct queue_limits *limits;
	u32 dev_flags = 0;
	int ret = -EINVAL;

	if (!ib_dev) {
		pr_err("Unable to locate struct iblock_dev parameter\n");
		return ERR_PTR(ret);
	}
	memset(&dev_limits, 0, sizeof(struct se_dev_limits));

	ib_dev->ibd_bio_set = bioset_create(IBLOCK_BIO_POOL_SIZE, 0);
	if (!ib_dev->ibd_bio_set) {
		pr_err("IBLOCK: Unable to create bioset()\n");
		return ERR_PTR(-ENOMEM);
	}
	pr_debug("IBLOCK: Created bio_set()\n");
	/*
	 * iblock_check_configfs_dev_params() ensures that ib_dev->ibd_udev_path
	 * must already have been set in order for echo 1 > $HBA/$DEV/enable to run.
	 */
	pr_debug( "IBLOCK: Claiming struct block_device: %s\n",
			ib_dev->ibd_udev_path);

	bd = blkdev_get_by_path(ib_dev->ibd_udev_path,
				FMODE_WRITE|FMODE_READ, ib_dev);
	if (IS_ERR(bd)) {
		ret = PTR_ERR(bd);
		goto failed;
	}
	/*
	 * Setup the local scope queue_limits from struct request_queue->limits
	 * to pass into transport_add_device_to_core_hba() as struct se_dev_limits.
	 */
	q = bdev_get_queue(bd);
	limits = &dev_limits.limits;
	limits->logical_block_size = bdev_logical_block_size(bd);
	limits->max_hw_sectors = queue_max_hw_sectors(q);
	limits->max_sectors = queue_max_sectors(q);
	dev_limits.hw_queue_depth = q->nr_requests;
	dev_limits.queue_depth = q->nr_requests;

	ib_dev->ibd_bd = bd;

	dev = transport_add_device_to_core_hba(hba,
			&iblock_template, se_dev, dev_flags, ib_dev,
			&dev_limits, pure_product_id, pure_revision);
	if (!dev)
		goto failed;

	/*
	 * Check if the underlying struct block_device request_queue supports
	 * the QUEUE_FLAG_DISCARD bit for UNMAP/WRITE_SAME in SCSI + TRIM
	 * in ATA and we need to set TPE=1
	 */
	if (blk_queue_discard(q)) {
		dev->se_sub_dev->se_dev_attrib.max_unmap_lba_count =
				q->limits.max_discard_sectors;
		/*
		 * Currently hardcoded to 1 in Linux/SCSI code..
		 */
		dev->se_sub_dev->se_dev_attrib.max_unmap_block_desc_count = 1;
		dev->se_sub_dev->se_dev_attrib.unmap_granularity =
				q->limits.discard_granularity >> 9;
		dev->se_sub_dev->se_dev_attrib.unmap_granularity_alignment =
				q->limits.discard_alignment;

		dev->se_sub_dev->se_dev_attrib.emulate_tpu = 1;
		dev->se_sub_dev->se_dev_attrib.emulate_tpws = 1;
	}

	if (blk_queue_nonrot(q))
		dev->se_sub_dev->se_dev_attrib.is_nonrot = 1;

	if (q->alloc_ps_buf_fn && q->exec_ps_buf_fn && q->free_ps_buf_fn)
		dev->dev_flags |= DF_USE_ALLOC_CMD_MEM;

	return dev;

failed:
	if (ib_dev->ibd_bio_set) {
		bioset_free(ib_dev->ibd_bio_set);
		ib_dev->ibd_bio_set = NULL;
	}
	ib_dev->ibd_bd = NULL;
	return ERR_PTR(ret);
}

static void iblock_free_device(void *p)
{
	struct iblock_dev *ib_dev = p;

	if (ib_dev->ibd_bd != NULL)
		blkdev_put(ib_dev->ibd_bd, FMODE_WRITE|FMODE_READ);
	if (ib_dev->ibd_bio_set != NULL)
		bioset_free(ib_dev->ibd_bio_set);
	kfree(ib_dev);
}

static inline struct iblock_req *IBLOCK_REQ(struct se_task *task)
{
	return container_of(task, struct iblock_req, ib_task);
}

static struct se_task *
iblock_alloc_task(unsigned char *cdb)
{
	struct iblock_req *ib_req;

	ib_req = kzalloc(sizeof(struct iblock_req), GFP_KERNEL);
	if (!ib_req) {
		pr_err("Unable to allocate memory for struct iblock_req\n");
		return NULL;
	}

	atomic_set(&ib_req->pending, 1);
	return &ib_req->ib_task;
}

static void iblock_ps_endio(struct ps_ioreq *iop, void *cmd_ptr, int error)
{
	struct se_cmd *cmd = cmd_ptr;
	/* We assume we only have one task if we're using ps_ioreqs */
	struct se_task *task = list_first_entry(&cmd->t_task_list, struct se_task, t_list);

	if (cmd->ps_opcode == PS_IO_COMPARE_AND_WRITE && error >= 0) {
		if (error < cmd->data_length) {
			u8 *buf = cmd->sense_buffer;

			cmd->scsi_sense_reason = TCM_MISCOMPARE_DURING_VERIFY;
			cmd->scsi_status = SAM_STAT_CHECK_CONDITION;
			cmd->scsi_sense_length = 18;

			/* CURRENT ERROR with VALID set*/
			buf[0] = 0x70 | 0x80;
			buf[SPC_ADD_SENSE_LEN_OFFSET] = 10;
			/* MISCOMPARE */
			buf[SPC_SENSE_KEY_OFFSET] = MISCOMPARE;
			/* MISCOMPARE DURING VERIFY OPERATION */
			buf[SPC_ASC_KEY_OFFSET] = 0x1D;
			buf[SPC_ASCQ_KEY_OFFSET] = 0x00;
			/* Miscompare offset in INFORMATION field */
			put_unaligned_be32(error, &buf[3]);
		}
		error = 0;
	}

	/* We assume we only have one task if we're using ps_ioreqs */
	transport_complete_task(task, !error);
}

static void iblock_ps_endpr(struct ps_ioreq *iop, void *cmd_ptr, int error)
{
	struct se_cmd *cmd = cmd_ptr;
	/* We assume we only have one task if we're using ps_ioreqs */
	struct se_task *task = list_first_entry(&cmd->t_task_list, struct se_task, t_list);

	if (error) {
		/* All errors are interpreted as a reservation conflict */
		cmd->se_cmd_flags |= SCF_SCSI_CDB_EXCEPTION;
		cmd->se_cmd_flags |= SCF_SCSI_RESERVATION_CONFLICT;
		cmd->scsi_sense_reason = TCM_RESERVATION_CONFLICT;

		transport_complete_task(task, 1);
		return;
	}

	/*
	 * Special case handling of REQUEST SENSE: if we get sense
	 * data back, we move it into the data in buffer to send back
	 * to the initiator that way.  In any case, we can complete
	 * the command with good status.
	 */
	if (cmd->t_task_cdb[0] == REQUEST_SENSE) {
		u8 *buf;

		buf = transport_kmap_data_sg(cmd);

		if (cmd->scsi_status) {
			memcpy(buf, cmd->sense_buffer, 18);
			cmd->scsi_status = 0;
		} else {
			/*
			 * CURRENT ERROR, NO SENSE
			 */
			buf[0] = 0x70;
			buf[SPC_SENSE_KEY_OFFSET] = NO_SENSE;

			/*
			 * NO ADDITIONAL SENSE INFORMATION
			 */
			buf[SPC_ASC_KEY_OFFSET] = 0x00;
			buf[7] = 0x0A;
		}

		transport_kunmap_data_sg(cmd);

		error = 18;
	} else {
		/*
		 * XXX: Is it ok to execute cmd->execute_task() from this
		 * context, or do some of them now assume they're run from the
		 * same context?
		 */
		if (cmd->execute_task)
			error = cmd->execute_task(task);
		else
			WARN(1, "iblock_ps_endpr called without execute_task");
	}

	pr_debug("%s cdb=%d scsi_status=%d error=%d data_length=%d "
		 "residual_count=%d se_cmd_flags=0x%x\n", __func__,
		 cmd->t_task_cdb[0], cmd->scsi_status, error,
		 cmd->data_length, cmd->residual_count, cmd->se_cmd_flags);

	if (error < 0) {
		unsigned long flags;

		spin_lock_irqsave(&cmd->t_state_lock, flags);
		task->task_flags &= ~TF_ACTIVE;
		cmd->transport_state &= ~CMD_T_SENT;
		spin_unlock_irqrestore(&cmd->t_state_lock, flags);

		/* There's only one task (us) and it's not really been started
		 * yet, so we don't need to stop it */
		transport_generic_request_failure(cmd);
	} else if (error > 0) {
		if (error < cmd->data_length) {
			if (cmd->se_cmd_flags & SCF_UNDERFLOW_BIT) {
				cmd->residual_count += cmd->data_length - error;
			} else {
				cmd->se_cmd_flags |= SCF_UNDERFLOW_BIT;
				cmd->residual_count = cmd->data_length - error;
			}

			cmd->data_length = error;
		}

		task->task_scsi_status = GOOD;
		transport_complete_task(task, 1);
	}
}

static struct ps_ioreq * iblock_ps_alloc(struct request_queue *q,
					 struct se_cmd *cmd, unsigned size,
					 int opcode, ps_buf_end_io_fn endio,
					 void *endio_priv)
{
	struct exec2_ps_nexus nexus;
	struct ps_ioreq *iop;

	target_session_i_t_nexus(cmd, &nexus.initiator,
				 &nexus.initiator_len, &nexus.target,
				 &nexus.target_len);

	if (cmd->alloc_cmd_mem_flags & CMD_A_FAIL_WHEN_EMPTY)
		opcode |= PS_IO_FLAG_NOWAIT;

	cmd->free_buf = q->free_ps_buf_fn;
	iop = q->alloc_ps_buf_fn(q, size, opcode, &cmd->t_data_sg,
				 &cmd->t_data_nents, &nexus, endio,
				 endio_priv);

	if (IS_ERR(iop)) {
		if (PTR_ERR(iop) == -EAGAIN) {
			WARN_ON(!(cmd->alloc_cmd_mem_flags & CMD_A_FAIL_WHEN_EMPTY));
			iop = ERR_PTR(-ENOMEM);
			cmd->alloc_cmd_mem_flags |= CMD_A_FAILED_EMPTY;
		}
	}

	return iop;
}

static int iblock_alloc_cmd_mem(struct se_cmd *cmd)
{
	struct iblock_dev *ib_dev = cmd->se_dev->dev_ptr;
	struct request_queue *q = bdev_get_queue(ib_dev->ibd_bd);
	struct ps_ioreq *iop;
	unsigned alloc_size;

	alloc_size = cmd->data_length;
	/*
	 * Special case: for UNMAP commands, the data_length will be
	 * the length of the descriptor.  We need to allocate enough
	 * space for the descriptor and also for the sector buffer we
	 * use for the write-same command we turn it into.
	 */
	if (cmd->ps_opcode == PS_IO_WRITE_SAME)
		alloc_size = max(alloc_size, 1u << 9);

	if ((cmd->se_cmd_flags & SCF_OFFLOAD_SCSI_RESERVATION) && !cmd->ps_opcode) {
		/* Flow control PR offload requests by ensuring we allocate at
		 * least some space from the buffer pool */
		iop = iblock_ps_alloc(q, cmd, alloc_size,
				      PS_IO_PR_OFFLOAD, iblock_ps_endpr, cmd);
	} else {
		iop = iblock_ps_alloc(q, cmd, alloc_size, cmd->ps_opcode, iblock_ps_endio, cmd);
	}

	if (IS_ERR(iop))
		return PTR_ERR(iop);
	else
		cmd->ps_iop = iop;

	return 0;
}

static void iblock_free_cmd_mem(struct se_cmd *cmd)
{
	if (cmd->free_buf)
		cmd->free_buf(cmd->ps_iop);
	else
		WARN_ON_ONCE(cmd->ps_iop);

	cmd->ps_iop = NULL;
}

static unsigned long long iblock_emulate_read_cap_with_block_size(
	struct se_device *dev,
	struct block_device *bd,
	struct request_queue *q)
{
	unsigned long long blocks_long = (div_u64(i_size_read(bd->bd_inode),
					bdev_logical_block_size(bd)) - 1);
	u32 block_size = bdev_logical_block_size(bd);

	if (block_size == dev->se_sub_dev->se_dev_attrib.block_size)
		return blocks_long;

	switch (block_size) {
	case 4096:
		switch (dev->se_sub_dev->se_dev_attrib.block_size) {
		case 2048:
			blocks_long <<= 1;
			break;
		case 1024:
			blocks_long <<= 2;
			break;
		case 512:
			blocks_long <<= 3;
		default:
			break;
		}
		break;
	case 2048:
		switch (dev->se_sub_dev->se_dev_attrib.block_size) {
		case 4096:
			blocks_long >>= 1;
			break;
		case 1024:
			blocks_long <<= 1;
			break;
		case 512:
			blocks_long <<= 2;
			break;
		default:
			break;
		}
		break;
	case 1024:
		switch (dev->se_sub_dev->se_dev_attrib.block_size) {
		case 4096:
			blocks_long >>= 2;
			break;
		case 2048:
			blocks_long >>= 1;
			break;
		case 512:
			blocks_long <<= 1;
			break;
		default:
			break;
		}
		break;
	case 512:
		switch (dev->se_sub_dev->se_dev_attrib.block_size) {
		case 4096:
			blocks_long >>= 3;
			break;
		case 2048:
			blocks_long >>= 2;
			break;
		case 1024:
			blocks_long >>= 1;
			break;
		default:
			break;
		}
		break;
	default:
		break;
	}

	return blocks_long;
}

static void iblock_end_io_flush(struct bio *bio, int err)
{
	struct se_cmd *cmd = bio->bi_private;

	if (err)
		pr_err("IBLOCK: cache flush failed: %d\n", err);

	if (cmd)
		transport_complete_sync_cache(cmd, err == 0);
	bio_put(bio);
}

/*
 * Implement SYCHRONIZE CACHE.  Note that we can't handle lba ranges and must
 * always flush the whole cache.
 */
static void iblock_emulate_sync_cache(struct se_task *task)
{
	struct se_cmd *cmd = task->task_se_cmd;
	struct iblock_dev *ib_dev = cmd->se_dev->dev_ptr;
	int immed = (cmd->t_task_cdb[1] & 0x2);
	struct bio *bio;

	/*
	 * If the Immediate bit is set, queue up the GOOD response
	 * for this SYNCHRONIZE_CACHE op.
	 */
	if (immed)
		transport_complete_sync_cache(cmd, 1);

	bio = bio_alloc(GFP_KERNEL, 0);
	bio->bi_end_io = iblock_end_io_flush;
	bio->bi_bdev = ib_dev->ibd_bd;
	if (!immed)
		bio->bi_private = cmd;
	submit_bio(WRITE_FLUSH, bio);
}

static int iblock_do_discard(struct se_device *dev, sector_t lba, u32 range)
{
	struct iblock_dev *ibd = dev->dev_ptr;
	struct block_device *bd = ibd->ibd_bd;
	int barrier = 0;

	return blkdev_issue_discard(bd, lba, range, GFP_KERNEL, barrier);
}

static void iblock_free_task(struct se_task *task)
{
	kfree(IBLOCK_REQ(task));
}

enum {
	Opt_udev_path, Opt_force, Opt_err
};

static match_table_t tokens = {
	{Opt_udev_path, "udev_path=%s"},
	{Opt_force, "force=%d"},
	{Opt_err, NULL}
};

static ssize_t iblock_set_configfs_dev_params(struct se_hba *hba,
					       struct se_subsystem_dev *se_dev,
					       const char *page, ssize_t count)
{
	struct iblock_dev *ib_dev = se_dev->se_dev_su_ptr;
	char *orig, *ptr, *arg_p, *opts;
	substring_t args[MAX_OPT_ARGS];
	int ret = 0, token;

	opts = kstrdup(page, GFP_KERNEL);
	if (!opts)
		return -ENOMEM;

	orig = opts;

	while ((ptr = strsep(&opts, ",\n")) != NULL) {
		if (!*ptr)
			continue;

		token = match_token(ptr, tokens, args);
		switch (token) {
		case Opt_udev_path:
			if (ib_dev->ibd_bd) {
				pr_err("Unable to set udev_path= while"
					" ib_dev->ibd_bd exists\n");
				ret = -EEXIST;
				goto out;
			}
			arg_p = match_strdup(&args[0]);
			if (!arg_p) {
				ret = -ENOMEM;
				break;
			}
			snprintf(ib_dev->ibd_udev_path, SE_UDEV_PATH_LEN,
					"%s", arg_p);
			kfree(arg_p);
			pr_debug("IBLOCK: Referencing UDEV path: %s\n",
					ib_dev->ibd_udev_path);
			ib_dev->ibd_flags |= IBDF_HAS_UDEV_PATH;
			break;
		case Opt_force:
			break;
		default:
			break;
		}
	}

out:
	kfree(orig);
	return (!ret) ? count : ret;
}

static ssize_t iblock_check_configfs_dev_params(
	struct se_hba *hba,
	struct se_subsystem_dev *se_dev)
{
	struct iblock_dev *ibd = se_dev->se_dev_su_ptr;

	if (!(ibd->ibd_flags & IBDF_HAS_UDEV_PATH)) {
		pr_err("Missing udev_path= parameters for IBLOCK\n");
		return -EINVAL;
	}

	return 0;
}

static ssize_t iblock_show_configfs_dev_params(
	struct se_hba *hba,
	struct se_subsystem_dev *se_dev,
	char *b)
{
	struct iblock_dev *ibd = se_dev->se_dev_su_ptr;
	struct block_device *bd = ibd->ibd_bd;
	char buf[BDEVNAME_SIZE];
	ssize_t bl = 0;

	if (bd)
		bl += sprintf(b + bl, "iBlock device: %s",
				bdevname(bd, buf));
	if (ibd->ibd_flags & IBDF_HAS_UDEV_PATH) {
		bl += sprintf(b + bl, "  UDEV PATH: %s\n",
				ibd->ibd_udev_path);
	} else
		bl += sprintf(b + bl, "\n");

	bl += sprintf(b + bl, "        ");
	if (bd) {
		bl += sprintf(b + bl, "Major: %d Minor: %d  %s\n",
			MAJOR(bd->bd_dev), MINOR(bd->bd_dev), (!bd->bd_contains) ?
			"" : (bd->bd_holder == ibd) ?
			"CLAIMED: IBLOCK" : "CLAIMED: OS");
	} else {
		bl += sprintf(b + bl, "Major: 0 Minor: 0\n");
	}

	return bl;
}

static void iblock_bio_destructor(struct bio *bio)
{
	struct se_task *task = bio->bi_private;
	struct iblock_dev *ib_dev = task->task_se_cmd->se_dev->dev_ptr;

	bio_free(bio, ib_dev->ibd_bio_set);
}

static struct bio *
iblock_get_bio(struct se_task *task, sector_t lba, u32 sg_num)
{
	struct iblock_dev *ib_dev = task->task_se_cmd->se_dev->dev_ptr;
	struct iblock_req *ib_req = IBLOCK_REQ(task);
	struct bio *bio;

	/*
	 * Only allocate as many vector entries as the bio code allows us to,
	 * we'll loop later on until we have handled the whole request.
	 */
	if (sg_num > BIO_MAX_PAGES)
		sg_num = BIO_MAX_PAGES;

	bio = bio_alloc_bioset(GFP_NOIO, sg_num, ib_dev->ibd_bio_set);
	if (!bio) {
		pr_err("Unable to allocate memory for bio (sg_num %d, dev %s)\n",
		       sg_num, ib_dev->ibd_udev_path);
		return NULL;
	}

	pr_debug("Allocated bio: %p task_sg_nents: %u using ibd_bio_set:"
		" %p\n", bio, task->task_sg_nents, ib_dev->ibd_bio_set);
	pr_debug("Allocated bio: %p task_size: %u\n", bio, task->task_size);

	bio->bi_bdev = ib_dev->ibd_bd;
	bio->bi_private = task;
	bio->bi_destructor = iblock_bio_destructor;
	bio->bi_end_io = &iblock_bio_done;
	bio->bi_sector = lba;
	atomic_inc(&ib_req->pending);

	pr_debug("Set bio->bi_sector: %llu\n", (unsigned long long)bio->bi_sector);
	pr_debug("Set ib_req->pending: %d\n", atomic_read(&ib_req->pending));
	return bio;
}

static void iblock_submit_bios(struct bio_list *list, int rw)
{
	struct blk_plug plug;
	struct bio *bio;

	blk_start_plug(&plug);
	while ((bio = bio_list_pop(list)))
		submit_bio(rw, bio);
	blk_finish_plug(&plug);
}

static void iblock_ps_exec(struct request_queue *q, struct se_cmd *cmd,
			   sector_t sect, u64 xparam)
{
	cmd->scsi_status = 0;
	cmd->scsi_sense_length = 0;
	q->exec_ps_buf_fn(cmd->ps_iop, sect, xparam, cmd->recv_time,
			  cmd->sense_buffer, &cmd->scsi_status,
			  &cmd->scsi_sense_length);
}

static int iblock_do_task(struct se_task *task)
{
	struct se_cmd *cmd = task->task_se_cmd;
	struct se_device *dev = cmd->se_dev;
	struct iblock_req *ibr = IBLOCK_REQ(task);
	struct bio *bio;
	struct bio_list list;
	struct scatterlist *sg;
	u32 i, sg_num = task->task_sg_nents;
	sector_t block_lba;
	unsigned bio_cnt;
	int rw;

	if (cmd->ps_iop) {
		struct iblock_dev *ib_dev = dev->dev_ptr;
		struct request_queue *q = bdev_get_queue(ib_dev->ibd_bd);

		iblock_ps_exec(q, cmd, cmd->t_task_lba, 0);
		return 0;
	}

	if (task->task_data_direction == DMA_TO_DEVICE) {
		/*
		 * Force data to disk if we pretend to not have a volatile
		 * write cache, or the initiator set the Force Unit Access bit.
		 */
		if (dev->se_sub_dev->se_dev_attrib.emulate_write_cache == 0 ||
		    (dev->se_sub_dev->se_dev_attrib.emulate_fua_write > 0 &&
		     (cmd->se_cmd_flags & SCF_FUA)))
			rw = WRITE_FUA;
		else
			rw = WRITE;
	} else {
		rw = READ;
	}

	/*
	 * Do starting conversion up from non 512-byte blocksize with
	 * struct se_task SCSI blocksize into Linux/Block 512 units for BIO.
	 */
	if (dev->se_sub_dev->se_dev_attrib.block_size == 4096)
		block_lba = (task->task_lba << 3);
	else if (dev->se_sub_dev->se_dev_attrib.block_size == 2048)
		block_lba = (task->task_lba << 2);
	else if (dev->se_sub_dev->se_dev_attrib.block_size == 1024)
		block_lba = (task->task_lba << 1);
	else if (dev->se_sub_dev->se_dev_attrib.block_size == 512)
		block_lba = task->task_lba;
	else {
		pr_err("Unsupported SCSI -> BLOCK LBA conversion:"
				" %u\n", dev->se_sub_dev->se_dev_attrib.block_size);
		cmd->scsi_sense_reason = TCM_LOGICAL_UNIT_COMMUNICATION_FAILURE;
		return -ENOSYS;
	}

	bio = iblock_get_bio(task, block_lba, sg_num);
	if (!bio) {
		cmd->scsi_sense_reason = TCM_LOGICAL_UNIT_COMMUNICATION_FAILURE;
		return -ENOMEM;
	}

	bio_list_init(&list);
	bio_list_add(&list, bio);
	bio_cnt = 1;

	for_each_sg(task->task_sg, sg, task->task_sg_nents, i) {
		/*
		 * XXX: if the length the device accepts is shorter than the
		 *	length of the S/G list entry this will cause and
		 *	endless loop.  Better hope no driver uses huge pages.
		 */
		while (bio_add_page(bio, sg_page(sg), sg->length, sg->offset)
				!= sg->length) {
			if (bio_cnt >= IBLOCK_MAX_BIO_PER_TASK) {
				iblock_submit_bios(&list, rw);
				bio_cnt = 0;
			}

			bio = iblock_get_bio(task, block_lba, sg_num);
			if (!bio)
				goto fail;
			bio_list_add(&list, bio);
			bio_cnt++;
		}

		/* Always in 512 byte units for Linux/Block */
		block_lba += sg->length >> IBLOCK_LBA_SHIFT;
		sg_num--;
	}

	iblock_submit_bios(&list, rw);

	if (atomic_dec_and_test(&ibr->pending)) {
		transport_complete_task(task,
				!atomic_read(&ibr->ib_bio_err_cnt));
	}
	return 0;

fail:
	while ((bio = bio_list_pop(&list)))
		bio_put(bio);
	cmd->scsi_sense_reason = TCM_LOGICAL_UNIT_COMMUNICATION_FAILURE;
	return -ENOMEM;
}

static int iblock_do_write_same(struct se_task *task, sector_t lba, u32 range)
{
	struct se_cmd *cmd = task->task_se_cmd;
	struct se_device *dev = cmd->se_dev;
	struct iblock_req *ibr = IBLOCK_REQ(task);
	struct iblock_dev *ibd = dev->dev_ptr;
	struct block_device *bd = ibd->ibd_bd;
	struct request_queue *q = bd->bd_disk->queue;
	struct bio *bio;
	struct scatterlist *sg;
	u32 i, sg_num = task->task_sg_nents;
	sector_t block_lba;
	int ret;

	/*
	 * Do starting conversion up from non 512-byte blocksize with
	 * struct se_task SCSI blocksize into Linux/Block 512 units for BIO.
	 */
	if (dev->se_sub_dev->se_dev_attrib.block_size == 4096)
		block_lba = (lba << 3);
	else if (dev->se_sub_dev->se_dev_attrib.block_size == 2048)
		block_lba = (lba << 2);
	else if (dev->se_sub_dev->se_dev_attrib.block_size == 1024)
		block_lba = (lba << 1);
	else if (dev->se_sub_dev->se_dev_attrib.block_size == 512)
		block_lba = lba;
	else {
		pr_err("Unsupported SCSI -> BLOCK LBA conversion:"
				" %u\n", dev->se_sub_dev->se_dev_attrib.block_size);
		cmd->scsi_sense_reason = TCM_LOGICAL_UNIT_COMMUNICATION_FAILURE;
		return -ENOSYS;
	}

	if (cmd->ps_iop) {
		iblock_ps_exec(bdev_get_queue(bd), cmd, block_lba, range << 9);
		return 0;
	}

	if (!q->write_same_fn) {
		cmd->scsi_sense_reason = TCM_UNSUPPORTED_SCSI_OPCODE;
		return -EIO;
	}

	bio = iblock_get_bio(task, block_lba, sg_num);
	if (!bio) {
		cmd->scsi_sense_reason = TCM_LOGICAL_UNIT_COMMUNICATION_FAILURE;
		return -ENOMEM;
	}

	for_each_sg(task->task_sg, sg, task->task_sg_nents, i)
		/* the data should fit in a single BIO.  Fail if it doesn't. */
		if (bio_add_page(bio, sg_page(sg), sg->length, sg->offset)
		    != sg->length) {
			pr_err("write_same buffer spans more than one bio.\n");
			goto fail;
		}

	pr_debug("Submitting write_same: %p bio: %p"
		 " bio->bi_sector: %llu\n", task, bio, (unsigned long long) bio->bi_sector);
	ret = q->write_same_fn(q, block_lba, range, bio);

	if (atomic_dec_and_test(&ibr->pending)) {
		transport_complete_task(task,
				!atomic_read(&ibr->ib_bio_err_cnt));
	}

	return ret;

fail:
	bio_put(bio);
	cmd->scsi_sense_reason = TCM_LOGICAL_UNIT_COMMUNICATION_FAILURE;
	return -ENOMEM;
}

static int iblock_do_compare_and_write(struct se_task *task, u32 range)
{
	struct se_cmd *cmd = task->task_se_cmd;
	struct se_device *dev = cmd->se_dev;
	struct iblock_req *ib_req = IBLOCK_REQ(task);
	struct iblock_dev *ibd = dev->dev_ptr;
	struct block_device *bd = ibd->ibd_bd;
	struct request_queue *q = bd->bd_disk->queue;
	struct bio *bio;
	struct scatterlist *sg;
	u32 i, sg_num = task->task_sg_nents;
	sector_t block_lba;
	unsigned int bio_size;
	int ret;

	/*
	 * Do starting conversion up from non 512-byte blocksize with
	 * struct se_task SCSI blocksize into Linux/Block 512 units for BIO.
	 */
	if (dev->se_sub_dev->se_dev_attrib.block_size == 4096)
		block_lba = (task->task_lba << 3);
	else if (dev->se_sub_dev->se_dev_attrib.block_size == 2048)
		block_lba = (task->task_lba << 2);
	else if (dev->se_sub_dev->se_dev_attrib.block_size == 1024)
		block_lba = (task->task_lba << 1);
	else if (dev->se_sub_dev->se_dev_attrib.block_size == 512)
		block_lba = task->task_lba;
	else {
		pr_err("Unsupported SCSI -> BLOCK LBA conversion:"
				" %u\n", dev->se_sub_dev->se_dev_attrib.block_size);
		cmd->scsi_sense_reason = TCM_LOGICAL_UNIT_COMMUNICATION_FAILURE;
		return -ENOSYS;
	}

	if (cmd->ps_iop) {
		iblock_ps_exec(bdev_get_queue(bd), cmd, block_lba, 0);
		return 0;
	}

	if (!q->compare_and_write_fn) {
		cmd->scsi_sense_reason = TCM_UNSUPPORTED_SCSI_OPCODE;
		return -EIO;
	}

	bio = iblock_get_bio(task, block_lba, sg_num);
	if (!bio) {
		cmd->scsi_sense_reason = TCM_LOGICAL_UNIT_COMMUNICATION_FAILURE;
		return -ENOMEM;
	}

	/*
	 * Use an alternate bio_done callback that can return extended
	 * sense data.
	 */
	bio->bi_end_io = &iblock_caw_bio_done;

	for_each_sg(task->task_sg, sg, task->task_sg_nents, i)
		/* the data should fit in a single BIO.  Fail if it doesn't. */
		if (bio_add_page(bio, sg_page(sg), sg->length, sg->offset)
		    != sg->length) {
			pr_err("compare_and_write buffer spans more than one bio.\n");
			goto fail;
		}

	bio_size = bio->bi_size;

	pr_debug("Submitting compare_and_write: %p bio: %p"
		 " bio->bi_sector: %llu\n", task, bio, (unsigned long long) bio->bi_sector);
	ret = q->compare_and_write_fn(q, block_lba, range, bio, &ib_req->cmd_retval);

	if (atomic_dec_and_test(&ib_req->pending)) {
		/* Return failure status for task if ib_bio_err_cnt > 0. */
		if (atomic_read(&ib_req->ib_bio_err_cnt))
			transport_complete_task(task, 0);
		/* If cmd_retval < bio length, return miscompare */
		else if (ib_req->cmd_retval < bio_size) {
			/* printk("retiring caw with retval %llu\n", ib_req->cmd_retval); */
			cmd->private = ib_req->cmd_retval;
			cmd->scsi_sense_reason = TCM_MISCOMPARE_DURING_VERIFY;
			transport_complete_task(task, 0);
		} else {
			/* Return GOOD status */
			transport_complete_task(task, 1);
		}
	}

	return ret;

fail:
	bio_put(bio);
	cmd->scsi_sense_reason = TCM_LOGICAL_UNIT_COMMUNICATION_FAILURE;
	return -ENOMEM;
}

static int iblock_do_pr_offload(struct se_task *task)
{
	struct se_cmd *cmd = task->task_se_cmd;
	struct se_device *dev = cmd->se_dev;

	if (cmd->ps_iop) {
		struct iblock_dev *ib_dev = dev->dev_ptr;
		struct request_queue *q = bdev_get_queue(ib_dev->ibd_bd);

		iblock_ps_exec(q, cmd, cmd->t_task_lba, cmd->t_task_cdb[0] | (cmd->t_task_cdb[1] << 8));
		return 0;
	}

	return -EACCES;
}

static int iblock_do_persistent_reserve(struct se_task *task, u8 sa, u8 scope, u8 type)
{
	struct se_cmd *cmd = task->task_se_cmd;
	struct se_device *dev = cmd->se_dev;
	union {
		u64		xparam;
		struct {
			u8	sa;
			u8	scope;
			u8	type;
		};
	} u;

	if (cmd->ps_iop) {
		struct iblock_dev *ib_dev = dev->dev_ptr;
		struct request_queue *q = bdev_get_queue(ib_dev->ibd_bd);

		u.sa = sa;
		u.scope = scope;
		u.type = type;

		iblock_ps_exec(q, cmd, 0, u.xparam);
		return 0;
	}

	return -ENOSYS;
}

static void iblock_ps_end_lun_reset(struct ps_ioreq *iop, void *tmr_ptr, int error)
{
	struct se_tmr_req * tmr = tmr_ptr;
	struct se_cmd *cmd = tmr->task_cmd;

	complete(tmr->offload_completion);
	iblock_free_cmd_mem(cmd);
}

static int iblock_do_lun_reset(struct se_tmr_req * tmr, struct completion *completion)
{
	struct se_cmd *cmd = tmr->task_cmd;
	struct iblock_dev *ib_dev = cmd->se_dev->dev_ptr;
	struct request_queue *q = bdev_get_queue(ib_dev->ibd_bd);
	struct ps_ioreq *iop;

	iop = iblock_ps_alloc(q, cmd, cmd->data_length, PS_IO_LUN_RESET,
			      iblock_ps_end_lun_reset, tmr);
	if (IS_ERR(iop))
		return PTR_ERR(iop);
	cmd->ps_iop = iop;
	tmr->offload_completion = completion;
	iblock_ps_exec(q, cmd, 0, 0);
	return 0;
}

static u32 iblock_get_device_rev(struct se_device *dev)
{
	return SCSI_SPC_3; /* Return SPC-4 in Initiator Data */
}

static u32 iblock_get_device_type(struct se_device *dev)
{
	return TYPE_DISK;
}

static const char * iblock_get_device_volume_name(struct se_device *dev)
{
	struct iblock_dev *ibd = dev->dev_ptr;
	struct block_device *bd = ibd->ibd_bd;
	struct request_queue *q = bdev_get_queue(bd);

	if (q->get_ps_volname_fn)
		return q->get_ps_volname_fn(q);
	return NULL;
}

static sector_t iblock_get_blocks(struct se_device *dev)
{
	struct iblock_dev *ibd = dev->dev_ptr;
	struct block_device *bd = ibd->ibd_bd;
	struct request_queue *q = bdev_get_queue(bd);

	return iblock_emulate_read_cap_with_block_size(dev, bd, q);
}

static void iblock_bio_done(struct bio *bio, int err)
{
	struct se_task *task = bio->bi_private;
	struct iblock_req *ibr = IBLOCK_REQ(task);

	/*
	 * Set -EIO if !BIO_UPTODATE and the passed is still err=0
	 */
	if (!test_bit(BIO_UPTODATE, &bio->bi_flags) && !err)
		err = -EIO;

	if (err != 0) {
		pr_err("IO error for bio: se_cmd %p, err %d, pending %d\n",
		       task->task_se_cmd, err, atomic_read(&ibr->pending));
		/*
		 * Bump the ib_bio_err_cnt and release bio.
		 */
		atomic_inc(&ibr->ib_bio_err_cnt);
		smp_mb__after_atomic_inc();
	}

	bio_put(bio);

	if (!atomic_dec_and_test(&ibr->pending))
		return;

	pr_debug("done[%p] bio: %p task_lba: %llu bio_lba: %llu err=%d\n",
		 task, bio, task->task_lba,
		 (unsigned long long)bio->bi_sector, err);

	transport_complete_task(task, !atomic_read(&ibr->ib_bio_err_cnt));
}

/*
 * Specialized bio_done for compare_and_write, because it needs
 * to return special SCSI sense data.
 */
static void iblock_caw_bio_done(struct bio *bio, int err)
{
	/* Starting here, this function is nearly identical to iblock_bio_done() */
	struct se_task *task = bio->bi_private;
	struct se_cmd *cmd = task->task_se_cmd;
	struct iblock_req *ibr = IBLOCK_REQ(task);
	unsigned int bio_size;

	/*
	 * Set -EIO if !BIO_UPTODATE and the passed is still err=0
	 */
	if (!(test_bit(BIO_UPTODATE, &bio->bi_flags)) && !(err))
		err = -EIO;

	if (err != 0) {
		pr_err("CAW IO error for bio: se_cmd %p, err %d, pending %d\n",
		       task->task_se_cmd, err, atomic_read(&ibr->pending));
		/*
		 * Bump the ib_bio_err_cnt and release bio.
		 */
		atomic_inc(&ibr->ib_bio_err_cnt);
		smp_mb__after_atomic_inc();
	}

	bio_size = bio->bi_size;
	bio_put(bio);

	if (!atomic_dec_and_test(&ibr->pending))
		return;

	pr_debug("done[%p] bio: %p task_lba: %llu bio_lba: %llu err=%d\n",
		 task, bio, task->task_lba,
		 (unsigned long long)bio->bi_sector, err);

	/* This is the end of similarity with iblock_bio_done() */

	/* Return failure status for task if ib_bio_err_cnt > 0. */
	if (atomic_read(&ibr->ib_bio_err_cnt))
		transport_complete_task(task, 0);

	/* If cmd_retval < bio length, return miscompare */
	else if (ibr->cmd_retval < bio_size) {
		/* printk("retiring caw with retval %llu\n", ibr->cmd_retval); */
		cmd->private = ibr->cmd_retval;
		cmd->scsi_sense_reason = TCM_MISCOMPARE_DURING_VERIFY;
		transport_complete_task(task, 0);
	} else {
		/* Return GOOD status */
		transport_complete_task(task, 1);
	}
}

static struct se_subsystem_api iblock_template = {
	.name			= "iblock",
	.owner			= THIS_MODULE,
	.transport_type		= TRANSPORT_PLUGIN_VHBA_PDEV,
	.write_cache_emulated	= 1,
	.fua_write_emulated	= 1,
	.attach_hba		= iblock_attach_hba,
	.detach_hba		= iblock_detach_hba,
	.allocate_virtdevice	= iblock_allocate_virtdevice,
	.create_virtdevice	= iblock_create_virtdevice,
	.free_device		= iblock_free_device,
	.alloc_task		= iblock_alloc_task,
	.alloc_cmd_mem		= iblock_alloc_cmd_mem,
	.free_cmd_mem		= iblock_free_cmd_mem,
	.do_pr_offload		= iblock_do_pr_offload,
	.do_persistent_reserve	= iblock_do_persistent_reserve,
	.do_task		= iblock_do_task,
	.do_discard		= iblock_do_discard,
	 /* FIXME: make conditional on pure device? */
	.do_write_same          = iblock_do_write_same,
	.do_compare_and_write   = iblock_do_compare_and_write,
	.do_lun_reset		= iblock_do_lun_reset,
	.do_sync_cache		= iblock_emulate_sync_cache,
	.free_task		= iblock_free_task,
	.check_configfs_dev_params = iblock_check_configfs_dev_params,
	.set_configfs_dev_params = iblock_set_configfs_dev_params,
	.show_configfs_dev_params = iblock_show_configfs_dev_params,
	.get_device_rev		= iblock_get_device_rev,
	.get_device_type	= iblock_get_device_type,
	.get_blocks		= iblock_get_blocks,
	.get_volume_name        = iblock_get_device_volume_name,
};

static int __init iblock_module_init(void)
{
	return transport_subsystem_register(&iblock_template);
}

static void iblock_module_exit(void)
{
	transport_subsystem_release(&iblock_template);
}

MODULE_DESCRIPTION("TCM IBLOCK subsystem plugin");
MODULE_AUTHOR("nab@Linux-iSCSI.org");
MODULE_LICENSE("GPL");

module_init(iblock_module_init);
module_exit(iblock_module_exit);
