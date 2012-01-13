/*
 * SCSI block command processing
 *
 * Copyright (C) 2004-2007 FUJITA Tomonori <tomof@acm.org>
 * Copyright (C) 2005-2007 Mike Christie <michaelc@cs.wisc.edu>
 *
 * This code is based on Ardis's iSCSI implementation.
 *   http://www.ardistech.com/iscsi/
 *   Copyright (C) 2002-2003 Ardis Technolgies <roman@ardistech.com>,
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation, version 2 of the
 * License.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA
 * 02110-1301 USA
 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <linux/fs.h>

#include "list.h"
#include "util.h"
#include "tgtd.h"
#include "tgtadm_error.h"
#include "target.h"
#include "driver.h"
#include "scsi.h"
#include "spc.h"
#include "tgtadm_error.h"

#define DEFAULT_BLK_SHIFT 9

static unsigned int blk_shift = DEFAULT_BLK_SHIFT;

static int sbc_mode_page_update(struct scsi_cmd *cmd, uint8_t *data, int *changed)
{
	uint8_t pcode = data[0] & 0x3f;
	struct mode_pg *pg = cmd->dev->mode_pgs[pcode];
	uint8_t old;

	eprintf("%x %x\n", pg->mode_data[0], data[2]);

	if (pcode == 0x08) {
		old = pg->mode_data[0];
		if (0x4 & data[2])
			pg->mode_data[0] |= 0x4;
		else
			pg->mode_data[0] &= ~0x4;

		if (old != pg->mode_data[0])
			*changed = 1;

		return 0;
	}

	return 1;
}

static int sbc_mode_select(int host_no, struct scsi_cmd *cmd)
{
	return spc_mode_select(host_no, cmd, sbc_mode_page_update);
}

static int sbc_mode_sense(int host_no, struct scsi_cmd *cmd)
{
	int ret;

	ret = spc_mode_sense(host_no, cmd);

	/*
	 * If this is a read-only lun, we must modify the data and set the
	 * write protect bit
	 */
	if (cmd->dev->attrs.readonly && ret == SAM_STAT_GOOD) {
		uint8_t *data, mode6;

		mode6 = (cmd->scb[0] == 0x1a);
		data = scsi_get_in_buffer(cmd);

		if (mode6)
			data[2] |= 0x80;
		else
			data[3] |= 0x80;
	}

	return ret;
}

static int sbc_rw(int host_no, struct scsi_cmd *cmd)
{
	int ret;
	uint64_t lba;
	uint32_t tl;
	unsigned char key = ILLEGAL_REQUEST;
	uint16_t asc = ASC_LUN_NOT_SUPPORTED;
	struct scsi_lu *lu = cmd->dev;

	ret = device_reserved(cmd);
	if (ret)
		return SAM_STAT_RESERVATION_CONFLICT;

	switch (cmd->scb[0]) {
	case READ_10:
	case READ_12:
	case READ_16:
		if (cmd->scb[1] & 0xe0) {
			key = ILLEGAL_REQUEST;
			asc = ASC_INVALID_OP_CODE;
			goto sense;
		}
		break;
	}

	if (lu->attrs.readonly) {
		switch (cmd->scb[0]) {
		case WRITE_6:
		case WRITE_10:
		case WRITE_12:
		case WRITE_16:
			key = DATA_PROTECT;
			asc = ASC_WRITE_PROTECT;
			goto sense;
			break;
		}
	}

	lba = scsi_rw_offset(cmd->scb) << cmd->dev->blk_shift;
	tl  = scsi_rw_count(cmd->scb) << cmd->dev->blk_shift;

	/* Verify that we are not doing i/o beyond
	   the end-of-lun */
	if (tl && (lba + tl > lu->size)) {
		key = ILLEGAL_REQUEST;
		asc = ASC_LBA_OUT_OF_RANGE;
		goto sense;
	}

	cmd->offset = lba;

	ret = cmd->dev->bst->bs_cmd_submit(cmd);
	if (ret) {
		key = HARDWARE_ERROR;
		asc = ASC_INTERNAL_TGT_FAILURE;
	} else
		return SAM_STAT_GOOD;

sense:
	cmd->offset = 0;
	scsi_set_in_resid_by_actual(cmd, 0);
	scsi_set_out_resid_by_actual(cmd, 0);

	sense_data_build(cmd, key, asc);
	return SAM_STAT_CHECK_CONDITION;
}

static int sbc_reserve(int host_no, struct scsi_cmd *cmd)
{
	if (device_reserve(cmd))
		return SAM_STAT_RESERVATION_CONFLICT ;
	else
		return SAM_STAT_GOOD;
}

static int sbc_release(int host_no, struct scsi_cmd *cmd)
{
	int ret;

	ret = device_release(cmd->c_target->tid, cmd->cmd_itn_id,
			     cmd->dev->lun, 0);

	return ret ? SAM_STAT_RESERVATION_CONFLICT : SAM_STAT_GOOD;
}

static int sbc_read_capacity(int host_no, struct scsi_cmd *cmd)
{
	uint32_t *data;
	unsigned int bshift;
	uint64_t size;
	uint8_t *scb = cmd->scb;
	unsigned char key = ILLEGAL_REQUEST;
	uint16_t asc = ASC_LUN_NOT_SUPPORTED;

	if (!(scb[8] & 0x1) && (scb[2] | scb[3] | scb[4] | scb[5])) {
		asc = ASC_INVALID_FIELD_IN_CDB;
		goto sense;
	}

	if (scsi_get_in_length(cmd) < 8)
		goto overflow;

	data = scsi_get_in_buffer(cmd);
	bshift = cmd->dev->blk_shift;
	size = cmd->dev->size >> bshift;

	data[0] = (size >> 32) ?
		__cpu_to_be32(0xffffffff) : __cpu_to_be32(size - 1);
	data[1] = __cpu_to_be32(1U << bshift);

overflow:
	scsi_set_in_resid_by_actual(cmd, 8);
	return SAM_STAT_GOOD;
sense:
	scsi_set_in_resid_by_actual(cmd, 0);
	sense_data_build(cmd, key, asc);
	return SAM_STAT_CHECK_CONDITION;
}

static int sbc_verify(int host_no, struct scsi_cmd *cmd)
{
	return SAM_STAT_GOOD;
}

static int sbc_service_action(int host_no, struct scsi_cmd *cmd)
{
	uint32_t *data;
	unsigned int bshift;
	uint64_t size;
	int len = 32;
	int val;

	if (cmd->scb[1] != SAI_READ_CAPACITY_16)
		goto sense;

	if (scsi_get_in_length(cmd) < 16)
		goto overflow;

	len = min_t(int, len, scsi_get_in_length(cmd));

	data = scsi_get_in_buffer(cmd);
	memset(data, 0, len);

	bshift = cmd->dev->blk_shift;
	size = cmd->dev->size >> bshift;

	*((uint64_t *)(data)) = __cpu_to_be64(size - 1);
	data[2] = __cpu_to_be32(1UL << bshift);
	// XXX!
	uint8_t *buf=(uint8_t *)&data[3];
	buf[0]=0;
	buf[1]=3;
	buf[2]=0x80|0x40;
	buf[3]=0;

	val = (cmd->dev->attrs.lbppbe << 16) | cmd->dev->attrs.la_lba;
	data[3] = __cpu_to_be32(val);

overflow:
	scsi_set_in_resid_by_actual(cmd, len);
	return SAM_STAT_GOOD;
sense:
	sense_data_build(cmd, ILLEGAL_REQUEST, ASC_INVALID_OP_CODE);
	return SAM_STAT_CHECK_CONDITION;
}

static int sbc_sync_cache(int host_no, struct scsi_cmd *cmd)
{
	int ret, len;
	uint8_t key = ILLEGAL_REQUEST;
	uint16_t asc = ASC_LUN_NOT_SUPPORTED;

	if (device_reserved(cmd))
		return SAM_STAT_RESERVATION_CONFLICT;

	ret = cmd->dev->bst->bs_cmd_submit(cmd);
	switch (ret) {
	case EROFS:
	case EINVAL:
	case EBADF:
	case EIO:
		/*
		 * is this the right sense code?
		 * what should I put for the asc/ascq?
		 */
		key = HARDWARE_ERROR;
		asc = ASC_INTERNAL_TGT_FAILURE;
		goto sense;
	default:
		len = 0;
		return SAM_STAT_GOOD;
	}

sense:
	scsi_set_in_resid_by_actual(cmd, 0);
	sense_data_build(cmd, key, asc);
	return SAM_STAT_CHECK_CONDITION;
}

static int sbc_lu_init(struct scsi_lu *lu)
{
	uint64_t size;
	uint8_t *data;

	if (spc_lu_init(lu))
		return TGTADM_NOMEM;

	strncpy(lu->attrs.product_id, "VIRTUAL-DISK", sizeof(lu->attrs.product_id));
	lu->attrs.version_desc[0] = 0x04C0; /* SBC-3 no version claimed */
	lu->attrs.version_desc[1] = 0x0960; /* iSCSI */
	lu->attrs.version_desc[2] = 0x0300; /* SPC-3 */

	data = lu->mode_block_descriptor;

	if (!lu->blk_shift)
		lu->blk_shift = blk_shift; /* if unset, use default shift */
	size = lu->size >> lu->blk_shift; /* calculate size in blocks */

	*(uint32_t *)(data) = (size >> 32) ?
			__cpu_to_be32(0xffffffff) : __cpu_to_be32(size);
	*(uint32_t *)(data + 4) = __cpu_to_be32(1 << lu->blk_shift);

	/* Vendor uniq - However most apps seem to call for mode page 0*/
	add_mode_page(lu, "0:0:0");
	/* Disconnect page */
	add_mode_page(lu, "2:0:14:0x80:0x80:0:0xa:0:0:0:0:0:0:0:0:0:0");
	/* Caching Page */
	add_mode_page(lu, "8:0:18:0x14:0:0xff:0xff:0:0:"
		      "0xff:0xff:0xff:0xff:0x80:0x14:0:0:0:0:0:0");
	{
		uint8_t mask[18];
		memset(mask, 0, sizeof(mask));
		mask[0] = 0x4;

		set_mode_page_changeable_mask(lu, 8, mask);
	}

	/* Control page */
	add_mode_page(lu, "10:0:10:2:0x10:0:0:0:0:0:0:2:0");
	/* Informational Exceptions Control page */
	add_mode_page(lu, "0x1c:0:10:8:0:0:0:0:0:0:0:0:0");

	return 0;
}

static struct device_type_template sbc_template = {
	.type		= TYPE_DISK,
	.lu_init	= sbc_lu_init,
	.lu_config	= spc_lu_config,
	.lu_online	= spc_lu_online,
	.lu_offline	= spc_lu_offline,
	.lu_exit	= spc_lu_exit,
	.ops		= {
		{spc_test_unit,},
		{spc_illegal_op,},
		{spc_illegal_op,},
		{spc_request_sense,},
		{spc_illegal_op,},
		{spc_illegal_op,},
		{spc_illegal_op,},
		{spc_illegal_op,},

		{sbc_rw, NULL, PR_EA_FA|PR_EA_FN},
		{spc_illegal_op,},
		{sbc_rw, NULL, PR_WE_FA|PR_EA_FA|PR_WE_FN|PR_EA_FN},
		{spc_illegal_op,},
		{spc_illegal_op,},
		{spc_illegal_op,},
		{spc_illegal_op,},
		{spc_illegal_op,},

		/* 0x10 */
		{spc_illegal_op,},
		{spc_illegal_op,},
		{spc_inquiry,},
		{spc_illegal_op,},
		{spc_illegal_op,},
		{sbc_mode_select, NULL, PR_WE_FA|PR_EA_FA|PR_WE_FN|PR_EA_FN},
		{sbc_reserve,},
		{sbc_release,},

		{spc_illegal_op,},
		{spc_illegal_op,},
		{sbc_mode_sense, NULL, PR_WE_FA|PR_EA_FA|PR_WE_FN|PR_EA_FN},
		{spc_start_stop, NULL, PR_SPECIAL},
		{spc_illegal_op,},
		{spc_illegal_op,},
		{spc_illegal_op,},
		{spc_illegal_op,},

		/* 0x20 */
		{spc_illegal_op,},
		{spc_illegal_op,},
		{spc_illegal_op,},
		{spc_illegal_op,},
		{spc_illegal_op,},
		{sbc_read_capacity,},
		{spc_illegal_op,},
		{spc_illegal_op,},

		{sbc_rw, NULL, PR_EA_FA|PR_EA_FN},
		{spc_illegal_op,},
		{sbc_rw, NULL, PR_WE_FA|PR_EA_FA|PR_WE_FN|PR_EA_FN},
		{spc_illegal_op,},
		{spc_illegal_op,},
		{spc_illegal_op,},
		{spc_illegal_op,},
		{sbc_verify, NULL, PR_EA_FA|PR_EA_FN},

		/* 0x30 */
		{spc_illegal_op,},
		{spc_illegal_op,},
		{spc_illegal_op,},
		{spc_illegal_op,},
		{spc_illegal_op,},
		{sbc_sync_cache, NULL, PR_WE_FA|PR_EA_FA|PR_WE_FN|PR_EA_FN},
		{spc_illegal_op,},
		{spc_illegal_op,},

		{spc_illegal_op,},
		{spc_illegal_op,},
		{spc_illegal_op,},
		{spc_illegal_op,},
		{spc_illegal_op,},
		{spc_illegal_op,},
		{spc_illegal_op,},
		{spc_illegal_op,},

		[0x40 ... 0x4f] = {spc_illegal_op,},

		/* 0x50 */
		{spc_illegal_op,},
		{spc_illegal_op,},
		{spc_illegal_op,},
		{spc_illegal_op,},
		{spc_illegal_op,},
		{sbc_mode_select, NULL, PR_WE_FA|PR_EA_FA|PR_WE_FN|PR_EA_FN},
		{spc_illegal_op,},
		{spc_illegal_op,},

		{spc_illegal_op,},
		{spc_illegal_op,},
		{sbc_mode_sense, NULL, PR_WE_FA|PR_EA_FA|PR_WE_FN|PR_EA_FN},
		{spc_illegal_op,},
		{spc_illegal_op,},
		{spc_illegal_op,},
		{spc_service_action, persistent_reserve_in_actions,},
		{spc_service_action, persistent_reserve_out_actions,},

		[0x60 ... 0x7f] = {spc_illegal_op,},

		/* 0x80 */
		{spc_illegal_op,},
		{spc_illegal_op,},
		{spc_illegal_op,},
		{spc_illegal_op,},
		{spc_illegal_op,},
		{spc_illegal_op,},
		{spc_illegal_op,},
		{spc_illegal_op,},

		{sbc_rw, NULL, PR_EA_FA|PR_EA_FN},
		{spc_illegal_op,},
		{sbc_rw, NULL, PR_WE_FA|PR_EA_FA|PR_WE_FN|PR_EA_FN},
		{spc_illegal_op,},
		{spc_illegal_op,},
		{spc_illegal_op,},
		{spc_illegal_op,},
		{spc_illegal_op,},

		/* 0x90 */
		{spc_illegal_op,},
		{sbc_sync_cache, NULL, PR_WE_FA|PR_EA_FA|PR_WE_FN|PR_EA_FN},
		{spc_illegal_op,},
		{spc_illegal_op,},
		{spc_illegal_op,},
		{spc_illegal_op,},
		{spc_illegal_op,},
		{spc_illegal_op,},

		{spc_illegal_op,},
		{spc_illegal_op,},
		{spc_illegal_op,},
		{spc_illegal_op,},
		{spc_illegal_op,},
		{spc_illegal_op,},
		{sbc_service_action,},
		{spc_illegal_op,},

		/* 0xA0 */
		{spc_report_luns,},
		{spc_illegal_op,},
		{spc_illegal_op,},
		{spc_service_action, maint_in_service_actions,},
		{spc_illegal_op,},
		{spc_illegal_op,},
		{spc_illegal_op,},
		{spc_illegal_op,},

		{sbc_rw, NULL, PR_EA_FA|PR_EA_FN},
		{spc_illegal_op,},
		{sbc_rw, NULL, PR_WE_FA|PR_EA_FA|PR_WE_FN|PR_EA_FN},
		{spc_illegal_op,},
		{spc_illegal_op,},
		{spc_illegal_op,},
		{spc_illegal_op,},
		{spc_illegal_op,},

		[0xb0 ... 0xff] = {spc_illegal_op},
		[0x42] = {sbc_rw,},
		[0x93] = {sbc_rw,},
	}
};

__attribute__((constructor)) static void sbc_init(void)
{
	device_type_register(&sbc_template);
}
