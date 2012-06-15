/*******************************************************************************
 * Filename:  target_core_tmr.c
 *
 * This file contains SPC-3 task management infrastructure
 *
 * Copyright (c) 2009,2010 Rising Tide Systems
 * Copyright (c) 2009,2010 Linux-iSCSI.org
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

#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/list.h>
#include <linux/export.h>
#include <scsi/scsi.h>
#include <scsi/scsi_cmnd.h>

#include <target/target_core_base.h>
#include <target/target_core_backend.h>
#include <target/target_core_fabric.h>
#include <target/target_core_configfs.h>

#include "target_core_internal.h"
#include "target_core_alua.h"
#include "target_core_pr.h"

void core_tmr_req_init(
	struct se_cmd *se_cmd,
	void *fabric_tmr_ptr,
	u8 function)
{
	struct se_tmr_req *tmr = &se_cmd->se_tmr_req;

	se_cmd->se_cmd_flags |= SCF_SCSI_TMR_CDB;
	tmr->task_cmd = se_cmd;
	tmr->fabric_tmr_ptr = fabric_tmr_ptr;
	tmr->function = function;
}
EXPORT_SYMBOL(core_tmr_req_init);

static void core_tmr_handle_tas_abort(
	struct se_node_acl *tmr_nacl,
	struct se_cmd *cmd,
	int tas,
	int fe_count)
{
	if (!fe_count) {
		transport_cmd_finish_abort(cmd, 1);
		return;
	}
	/*
	 * TASK ABORTED status (TAS) bit support
	*/
	if ((tmr_nacl &&
	     (tmr_nacl == cmd->se_sess->se_node_acl)) || tas)
		transport_send_task_abort(cmd);

	transport_cmd_finish_abort(cmd, 0);
}

static int target_check_cdb_and_preempt(struct list_head *list,
		struct se_cmd *cmd)
{
	struct t10_pr_registration *reg;

	if (!list)
		return 0;
	list_for_each_entry(reg, list, pr_reg_abort_list) {
		if (reg->pr_res_key == cmd->pr_res_key)
			return 0;
	}

	return 1;
}

void core_tmr_abort_task(
	struct se_device *dev,
	struct se_tmr_req *tmr,
	struct se_session *se_sess)
{
	struct se_cmd *se_cmd, *tmp_cmd;
	unsigned long flags;
	int ref_tag;

	spin_lock_irqsave(&se_sess->sess_cmd_lock, flags);
	list_for_each_entry_safe(se_cmd, tmp_cmd,
			&se_sess->sess_cmd_list, se_cmd_list) {

		if (dev != se_cmd->se_dev)
			continue;
		ref_tag = se_cmd->se_tfo->get_task_tag(se_cmd);
		if (tmr->ref_task_tag != ref_tag)
			continue;

		printk("ABORT_TASK: Found referenced %s task_tag: %u\n",
			se_cmd->se_tfo->get_fabric_name(), ref_tag);

		spin_lock(&se_cmd->t_state_lock);
		if (se_cmd->transport_state & CMD_T_COMPLETE) {
			printk("ABORT_TASK: ref_tag: %u already complete, skipping\n", ref_tag);
			spin_unlock(&se_cmd->t_state_lock);
			spin_unlock_irqrestore(&se_sess->sess_cmd_lock, flags);
			goto out;
		}
		se_cmd->transport_state |= CMD_T_ABORTED;
		spin_unlock(&se_cmd->t_state_lock);

		list_del_init(&se_cmd->se_cmd_list);
		kref_get(&se_cmd->cmd_kref);
		spin_unlock_irqrestore(&se_sess->sess_cmd_lock, flags);

		cancel_work_sync(&se_cmd->work);
		transport_wait_for_tasks(se_cmd);
		/*
		 * Now send SAM_STAT_TASK_ABORTED status for the referenced
		 * se_cmd descriptor..
		 */
		transport_send_task_abort(se_cmd);
		/*
		 * Also deal with possible extra acknowledge reference..
		 */
		if (se_cmd->se_cmd_flags & SCF_ACK_KREF)
			target_put_sess_cmd(se_sess, se_cmd);

		target_put_sess_cmd(se_sess, se_cmd);

		printk("ABORT_TASK: Sending TMR_FUNCTION_COMPLETE for"
				" ref_tag: %d\n", ref_tag);
		tmr->response = TMR_FUNCTION_COMPLETE;
		return;
	}
	spin_unlock_irqrestore(&se_sess->sess_cmd_lock, flags);

out:
	printk("ABORT_TASK: Sending TMR_TASK_DOES_NOT_EXIST for ref_tag: %d\n",
			tmr->ref_task_tag);
	tmr->response = TMR_TASK_DOES_NOT_EXIST;
}

static void core_tmr_drain_cmd_list(
	struct se_device *dev,
	struct se_cmd *prout_cmd,
	struct se_node_acl *tmr_nacl,
	int tas,
	struct list_head *preempt_and_abort_list)
{
	LIST_HEAD(drain_cmd_list);
	struct se_queue_obj *qobj = &dev->dev_queue_obj;
	struct se_cmd *cmd, *tcmd;
	unsigned long flags;
	/*
	 * Release all commands remaining in the struct se_device cmd queue.
	 *
	 * This follows the same logic as above for the struct se_device
	 * struct se_task state list, where commands are returned with
	 * TASK_ABORTED status, if there is an outstanding $FABRIC_MOD
	 * reference, otherwise the struct se_cmd is released.
	 */
	spin_lock_irqsave(&qobj->cmd_queue_lock, flags);
	list_for_each_entry_safe(cmd, tcmd, &qobj->qobj_list, se_queue_node) {
		/*
		 * For PREEMPT_AND_ABORT usage, only process commands
		 * with a matching reservation key.
		 */
		if (target_check_cdb_and_preempt(preempt_and_abort_list, cmd))
			continue;
		/*
		 * Not aborting PROUT PREEMPT_AND_ABORT CDB..
		 */
		if (prout_cmd == cmd)
			continue;

		cmd->transport_state |= CMD_T_ABORTED;
		cmd->transport_state &= ~CMD_T_QUEUED;
		atomic_dec(&qobj->queue_cnt);
		list_move_tail(&cmd->se_queue_node, &drain_cmd_list);
	}

	while (!list_empty(&drain_cmd_list)) {
		cmd = list_first_entry(&drain_cmd_list, struct se_cmd, se_queue_node);
		list_del_init(&cmd->se_queue_node);
		spin_unlock_irqrestore(&qobj->cmd_queue_lock, flags);

		if (cmd->se_cmd_flags & SCF_SCSI_TMR_CDB) {
			struct se_tmr_req *tmr = &cmd->se_tmr_req;

			pr_debug("LUN_RESET: %s releasing TMR %p Function: 0x%02x,"
				 " Response: 0x%02x, t_state: %d\n",
				 (preempt_and_abort_list) ? "Preempt" : "", tmr,
				 tmr->function, tmr->response, cmd->t_state);
			transport_cmd_finish_abort(cmd, 1);
		} else {
			pr_debug("LUN_RESET: %s from Device Queue: cmd: %p t_state:"
				 " %d t_fe_count: %d\n", (preempt_and_abort_list) ?
				 "Preempt" : "", cmd, cmd->t_state,
				 atomic_read(&cmd->t_fe_count));
			core_tmr_handle_tas_abort(tmr_nacl, cmd, tas,
						  atomic_read(&cmd->t_fe_count));
		}

		spin_lock_irqsave(&qobj->cmd_queue_lock, flags);
	}
	spin_unlock_irqrestore(&qobj->cmd_queue_lock, flags);
}

int core_tmr_lun_reset(
        struct se_device *dev,
        struct se_tmr_req *tmr,
        struct list_head *preempt_and_abort_list,
        struct se_cmd *prout_cmd)
{
	DECLARE_COMPLETION_ONSTACK(reset_done);
	bool wait_for_transport;
	struct se_node_acl *tmr_nacl = NULL;
	struct se_portal_group *tmr_tpg = NULL;
	int tas, rc;
        /*
	 * TASK_ABORTED status bit, this is configurable via ConfigFS
	 * struct se_device attributes.  spc4r17 section 7.4.6 Control mode page
	 *
	 * A task aborted status (TAS) bit set to zero specifies that aborted
	 * tasks shall be terminated by the device server without any response
	 * to the application client. A TAS bit set to one specifies that tasks
	 * aborted by the actions of an I_T nexus other than the I_T nexus on
	 * which the command was received shall be completed with TASK ABORTED
	 * status (see SAM-4).
	 */
	tas = dev->se_sub_dev->se_dev_attrib.emulate_tas;
	/*
	 * Determine if this se_tmr is coming from a $FABRIC_MOD
	 * or struct se_device passthrough..
	 */
	if (tmr && tmr->task_cmd && tmr->task_cmd->se_sess) {
		tmr_nacl = tmr->task_cmd->se_sess->se_node_acl;
		tmr_tpg = tmr->task_cmd->se_sess->se_tpg;
		if (tmr_nacl && tmr_tpg) {
			pr_debug("LUN_RESET: TMR caller fabric: %s"
				" initiator port %s\n",
				tmr_tpg->se_tpg_tfo->get_fabric_name(),
				tmr_nacl->initiatorname);
		}
	}
	pr_debug("LUN_RESET: %s starting for [%s], tas: %d\n",
		(preempt_and_abort_list) ? "Preempt" : "TMR",
		dev->transport->name, tas);

	wait_for_transport = false;
	if (target_core_offload_pr && dev->transport->do_lun_reset) {
		rc = dev->transport->do_lun_reset(tmr, &reset_done);
		if (rc) {
			pr_err("LUN_RESET: transport rc %d\n", rc);
		} else {
			wait_for_transport = true;
		}
	}

	core_tmr_drain_cmd_list(dev, prout_cmd, tmr_nacl, tas,
				preempt_and_abort_list);
	/*
	 * Clear any legacy SPC-2 reservation when called during
	 * LOGICAL UNIT RESET
	 */
	if (!preempt_and_abort_list &&
	     (dev->dev_flags & DF_SPC2_RESERVATIONS)) {
		spin_lock(&dev->dev_reservation_lock);
		dev->dev_reserved_node_acl = NULL;
		dev->dev_flags &= ~DF_SPC2_RESERVATIONS;
		spin_unlock(&dev->dev_reservation_lock);
		pr_debug("LUN_RESET: SCSI-2 Released reservation\n");
	}

	if (wait_for_transport)
		wait_for_completion_interruptible(&reset_done);

	spin_lock_irq(&dev->stats_lock);
	dev->num_resets++;
	spin_unlock_irq(&dev->stats_lock);

	pr_debug("LUN_RESET: %s for [%s] Complete\n",
			(preempt_and_abort_list) ? "Preempt" : "TMR",
			dev->transport->name);
	return 0;
}

