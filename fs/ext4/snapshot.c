/*
 * linux/fs/ext4/snapshot.c
 *
 * Written by Amir Goldstein <amir73il@users.sf.net>, 2008
 *
 * Copyright (C) 2008-2011 CTERA Networks
 *
 * This file is part of the Linux kernel and is made available under
 * the terms of the GNU General Public License, version 2, or at your
 * option, any later version, incorporated herein by reference.
 *
 * Ext4 snapshots core functions.
 */

#include <linux/quotaops.h>
#include "snapshot.h"
#include "ext4.h"
#include "mballoc.h"

#define snapshot_debug_hl(n, f, a...) snapshot_debug_l(n, handle ? \
					IS_COWING(handle) : 0, f, ## a)

/*
 * ext4_snapshot_map_blocks() - helper function for
 * ext4_snapshot_test_and_cow().  Test if blocks are mapped in snapshot file.
 * If @block is not mapped and if @cmd is non zero, try to allocate @maxblocks.
 * Also used by ext4_snapshot_create() to pre-allocate snapshot blocks.
 *
 * Return values:
 * > 0 - no. of mapped blocks in snapshot file
 * = 0 - @block is not mapped in snapshot file
 * < 0 - error
 */
int ext4_snapshot_map_blocks(handle_t *handle, struct inode *inode,
			      ext4_snapblk_t block, unsigned long maxblocks,
			      ext4_fsblk_t *mapped, int cmd)
{
	int err;
	struct ext4_map_blocks map;

	map.m_lblk = SNAPSHOT_IBLOCK(block);
	map.m_len = maxblocks;

	err = ext4_map_blocks(handle, inode, &map, cmd);
	/*
	 * ext4_get_blocks_handle() returns number of blocks
	 * mapped. 0 in case of a HOLE.
	 */
	if (mapped && err > 0)
		*mapped = map.m_pblk;

	snapshot_debug_hl(4, "snapshot (%u) map_blocks "
			"[%lld/%lld] = [%lld/%lld] "
			"cmd=%d, maxblocks=%lu, mapped=%d\n",
			inode->i_generation,
			SNAPSHOT_BLOCK_TUPLE(block),
			SNAPSHOT_BLOCK_TUPLE(map.m_pblk),
			cmd, maxblocks, err);
	return err;
}

