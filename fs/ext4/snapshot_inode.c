/*
 * linux/fs/ext4/snapshot_inode.c
 *
 * Written by Amir Goldstein <amir73il@users.sf.net>, 2008
 *
 * Copyright (C) 2008-2011 CTERA Networks
 *
 * This file is part of the Linux kernel and is made available under
 * the terms of the GNU General Public License, version 2, or at your
 * option, any later version, incorporated herein by reference.
 *
 * Ext4 snapshots inode functions.
 */

#include <linux/module.h>
#include <linux/fs.h>
#include <linux/time.h>
#include <linux/jbd2.h>
#include <linux/highuid.h>
#include <linux/pagemap.h>
#include <linux/quotaops.h>
#include <linux/string.h>
#include <linux/buffer_head.h>
#include <linux/writeback.h>
#include <linux/pagevec.h>
#include <linux/mpage.h>
#include <linux/namei.h>
#include <linux/uio.h>
#include <linux/bio.h>
#include <linux/workqueue.h>
#include <linux/kernel.h>
#include <linux/slab.h>

#include "ext4_jbd2.h"
#include "xattr.h"
#include "acl.h"
#include "ext4_extents.h"

#include <trace/events/ext4.h>
#include "snapshot.h"
/*
 * ext4_snapshot_get_block_access() - called from ext4_snapshot_read_through()
 * on snapshot file access.
 * return value <0 indicates access not granted
 * return value 0 indicates snapshot inode read through access
 * in which case 'prev_snapshot' is pointed to the previous snapshot
 * on the list or set to NULL to indicate read through to block device.
 */
/*
 * In-memory snapshot list manipulation is protected by snapshot_mutex.
 * In this function we read the in-memory snapshot list without holding
 * snapshot_mutex, because we don't want to slow down snapshot read performance.
 * Following is a proof, that even though we don't hold snapshot_mutex here,
 * reading the list is safe from races with snapshot list delete and add (take).
 *
 * Proof of no race with snapshot delete:
 * --------------------------------------
 * We get here only when reading from an enabled snapshot or when reading
 * through from an enabled snapshot to a newer snapshot.  Snapshot delete
 * operation is only allowed for a disabled snapshot, when no older enabled
 * snapshot exists (i.e., the deleted snapshot in not 'in-use').  Hence,
 * read through is safe from races with snapshot list delete operations.
 *
 * Proof of no race with snapshot take:
 * ------------------------------------
 * Snapshot B take is composed of the following steps:
 * ext4_snapshot_create():
 * - Add snapshot B to head of list (active_snapshot is A).
 * - Allocate and copy snapshot B initial blocks.
 * ext4_snapshot_take():
 * - Freeze FS
 * - Clear snapshot A 'active' flag.
 * - Set snapshot B 'list'+'active' flags.
 * - Set snapshot B as active snapshot (active_snapshot=B).
 * - Unfreeze FS
 *
 * Note that we do not need to rely on correct order of instructions within
 * each of the functions above, but we can assume that Freeze FS will provide
 * a strong barrier between adding B to list and the ops inside snapshot_take.
 *
 * When reading from snapshot A during snapshot B take, we have 2 cases:
 * 1. is_active(A) is tested before setting active_snapshot=B -
 *    read through from A to block device.
 * 2. is_active(A) is tested after setting active_snapshot=B -
 *    read through from A to B.
 *
 * When reading from snapshot B during snapshot B take, we have 2 cases:
 * 1. B->flags and B->prev are read before adding B to list
 *    AND/OR before setting the 'list'+'active' flags -
 *    access to B denied.
 * 2. is_active(B) is tested after setting active_snapshot=B
 *    AND/OR after setting the 'list'+'active' flags -
 *    read through from B to block device.
 */
static int ext4_snapshot_get_block_access(struct inode *inode,
		struct inode **prev_snapshot)
{
	struct ext4_inode_info *ei = EXT4_I(inode);
	unsigned long flags = ext4_get_snapstate_flags(inode);
	struct list_head *prev = ei->i_snaplist.prev;

	if (!(flags & 1UL<<EXT4_SNAPSTATE_LIST))
		/* snapshot not on the list - read/write access denied */
		return -EPERM;

	*prev_snapshot = NULL;
	if (ext4_snapshot_is_active(inode) ||
			(flags & 1UL<<EXT4_SNAPSTATE_ACTIVE))
		/* read through from active snapshot to block device */
		return 0;

	if (prev == &ei->i_snaplist)
		/* not on snapshots list? */
		return -EIO;

	if (prev == &EXT4_SB(inode->i_sb)->s_snapshot_list)
		/* active snapshot not found on list? */
		return -EIO;

	/* read through to prev snapshot on the list */
	ei = list_entry(prev, struct ext4_inode_info, i_snaplist);
	*prev_snapshot = &ei->vfs_inode;

	if (!ext4_snapshot_file(*prev_snapshot))
		/* non snapshot file on the list? */
		return -EIO;

	return 0;
}

#ifdef CONFIG_EXT4_DEBUG
/*
 * ext4_snapshot_get_blockdev_access - get read through access to block device.
 * Sanity test to verify that the read block is allocated and not excluded.
 * This test has performance penalty and is only called if SNAPTEST_READ
 * is enabled.  An attempt to read through to block device of a non allocated
 * or excluded block may indicate a corrupted filesystem, corrupted snapshot
 * or corrupted exclude bitmap.  However, it may also be a read-ahead, which
 * was not implicitly requested by the user, so be sure to disable read-ahead
 * on block device (blockdev --setra 0 <bdev>) before enabling SNAPTEST_READ.
 *
 * Return values:
 * = 0 - block is allocated and not excluded
 * < 0 - error (or block is not allocated or excluded)
 */
static int ext4_snapshot_get_blockdev_access(struct super_block *sb,
				   struct buffer_head *bh)
{
	unsigned long block_group = SNAPSHOT_BLOCK_GROUP(bh->b_blocknr);
	ext4_grpblk_t bit = SNAPSHOT_BLOCK_GROUP_OFFSET(bh->b_blocknr);
	struct buffer_head *bitmap_bh;
	int err = 0;

	if (PageReadahead(bh->b_page))
		return 0;

	bitmap_bh = ext4_read_block_bitmap(sb, block_group);
	if (!bitmap_bh)
		return -EIO;

	if (!ext4_test_bit(bit, bitmap_bh->b_data)) {
		snapshot_debug(2, "warning: attempt to read through to "
				"non-allocated block [%d/%lu] - read ahead?\n",
				bit, block_group);
		brelse(bitmap_bh);
		return -EIO;
	}

	brelse(bitmap_bh);
	return err;
}
#endif

/*
 * ext4_snapshot_read_through - get snapshot image block.
 * On read of snapshot file, an unmapped block is a peephole to prev snapshot.
 * On read of active snapshot, an unmapped block is a peephole to the block
 * device.  On first block write, the peephole is filled forever.
 */
static int ext4_snapshot_read_through(struct inode *inode, sector_t iblock,
				      struct buffer_head *bh_result)
{
	int err;
	struct ext4_map_blocks map;
	struct inode *prev_snapshot;
	struct buffer_head *sbh = NULL;

	map.m_lblk = iblock;
	map.m_pblk = 0;
	map.m_len = bh_result->b_size >> inode->i_blkbits;

get_block:
	prev_snapshot = NULL;
	/* request snapshot file read access */
	err = ext4_snapshot_get_block_access(inode, &prev_snapshot);
	if (err < 0)
		return err;
	err = ext4_map_blocks(NULL, inode, &map, 0);
	snapshot_debug(4, "ext4_snapshot_read_through(%lld): block = "
		       "(%lld), err = %d\n prev_snapshot = %u",
		       (long long)iblock, map.m_pblk, err,
		       prev_snapshot ? prev_snapshot->i_generation : 0);
	if (err < 0)
		return err;
	if (!err && prev_snapshot) {
		/* hole in snapshot - check again with prev snapshot */
		inode = prev_snapshot;
		goto get_block;
	}
	if (!err)
		/* hole in active snapshot - read though to block device */
		return 0;

	map_bh(bh_result, inode->i_sb, map.m_pblk);
	bh_result->b_state = (bh_result->b_state & ~EXT4_MAP_FLAGS) |
		map.m_flags;

	/*
	 * On read of active snapshot, a mapped block may belong to a non
	 * completed COW operation.  Use the buffer cache to test this
	 * condition.  if (bh_result->b_blocknr == SNAPSHOT_BLOCK(iblock)),
	 * then this is either read through to block device or moved block.
	 * Either way, it is not a COWed block, so it cannot be pending COW.
	 */
	if (ext4_snapshot_is_active(inode) &&
	    bh_result->b_blocknr != SNAPSHOT_BLOCK(iblock))
		sbh = sb_find_get_block(inode->i_sb, bh_result->b_blocknr);
	if (!sbh)
		return 0;
	/* wait for pending COW to complete */
	ext4_snapshot_test_pending_cow(sbh, SNAPSHOT_BLOCK(iblock));
	lock_buffer(sbh);
	if (buffer_uptodate(sbh)) {
		/*
		 * Avoid disk I/O and copy out snapshot page directly
		 * from block device page when possible.
		 */
		BUG_ON(!sbh->b_page);
		BUG_ON(!bh_result->b_page);
		lock_buffer(bh_result);
		copy_highpage(bh_result->b_page, sbh->b_page);
		set_buffer_uptodate(bh_result);
		unlock_buffer(bh_result);
	} else if (buffer_dirty(sbh)) {
		/*
		 * If snapshot data buffer is dirty (just been COWed),
		 * then it is not safe to read it from disk yet.
		 * We shouldn't get here because snapshot data buffer
		 * only becomes dirty during COW and because we waited
		 * for pending COW to complete, which means that a
		 * dirty snapshot data buffer should be uptodate.
		 */
		WARN_ON(1);
	}
	unlock_buffer(sbh);
	brelse(sbh);
	return 0;
}

/*
 * Check if @block is a bitmap block of @group.
 * if @block is found to be a block/inode/exclude bitmap block, the return
 * value is one of the non-zero values below:
 */
#define BLOCK_BITMAP	1
#define INODE_BITMAP	2
#define EXCLUDE_BITMAP	3

static int ext4_snapshot_is_group_bitmap(struct super_block *sb,
		ext4_fsblk_t block, ext4_group_t group)
{
	struct ext4_group_desc *gdp;
	struct ext4_group_info *grp;
	ext4_fsblk_t bitmap_blk;

	gdp = ext4_get_group_desc(sb, group, NULL);
	grp = ext4_get_group_info(sb, group);
	if (!gdp || !grp)
		return 0;

	bitmap_blk = ext4_block_bitmap(sb, gdp);
	if (bitmap_blk == block)
		return BLOCK_BITMAP;
	bitmap_blk = ext4_inode_bitmap(sb, gdp);
	if (bitmap_blk == block)
		return INODE_BITMAP;
	bitmap_blk = ext4_exclude_bitmap(sb, gdp);
	if (bitmap_blk == block)
		return EXCLUDE_BITMAP;
	return 0;
}

/*
 * Check if @block is a bitmap block and of any block group.
 * if @block is found to be a bitmap block, @bitmap_group is set to the
 * block group described by the bitmap block.
 */
static int ext4_snapshot_is_bitmap(struct super_block *sb,
		ext4_fsblk_t block, ext4_group_t *bitmap_group)
{
	ext4_group_t group = SNAPSHOT_BLOCK_GROUP(block);
	ext4_group_t ngroups = ext4_get_groups_count(sb);
	int flex_groups = ext4_flex_bg_size(EXT4_SB(sb));
	int i, is_bitmap = 0;

	/*
	 * When block is in the first block group of a flex group, we need to
	 * check all group desc of the flex group.
	 * The exclude bitmap can potentially be allocated from any group, if
	 * exclude inode was added not on mkfs.  The worst case if we fail to
	 * identify a block as an exclude bitmap is that fsck sanity check of
	 * snapshots will fail, becasue exclude bitmap inside snapshot will
	 * not be all zeros.
	 */
	if (!EXT4_HAS_INCOMPAT_FEATURE(sb, EXT4_FEATURE_INCOMPAT_FLEX_BG) ||
			(group % flex_groups) != 0)
		flex_groups = 1;

	for (i = 0; i < flex_groups && group < ngroups; i++, group++) {
		is_bitmap = ext4_snapshot_is_group_bitmap(sb, block, group);
		if (is_bitmap)
			break;
	}
	*bitmap_group = group;
	return is_bitmap;
}

static int ext4_snapshot_get_block(struct inode *inode, sector_t iblock,
			struct buffer_head *bh_result, int create)
{
	ext4_group_t block_group;
	int err, is_bitmap;

	BUG_ON(create != 0);
	BUG_ON(buffer_tracked_read(bh_result));

	err = ext4_snapshot_read_through(inode, SNAPSHOT_IBLOCK(iblock),
					 bh_result);
	snapshot_debug(4, "ext4_snapshot_get_block(%lld): block = (%lld), "
		       "err = %d\n",
		       (long long)iblock, buffer_mapped(bh_result) ?
		       (long long)bh_result->b_blocknr : 0, err);
	if (err < 0)
		return err;

	if (!buffer_tracked_read(bh_result))
		return 0;

	/* Check for read through to block or exclude bitmap block */
	is_bitmap = ext4_snapshot_is_bitmap(inode->i_sb, bh_result->b_blocknr,
			&block_group);
	if (is_bitmap == BLOCK_BITMAP) {
		/* copy fixed block bitmap directly to page buffer */
		cancel_buffer_tracked_read(bh_result);
		/* cancel_buffer_tracked_read() clears mapped flag */
		set_buffer_mapped(bh_result);
		snapshot_debug(2, "fixing snapshot block bitmap #%u\n",
			       block_group);
		/*
		 * XXX: if we return unmapped buffer, the page will be zeroed
		 * but if we return mapped to block device and uptodate buffer
		 * next readpage may read directly from block device without
		 * fixing block bitmap.  This only affects fsck of snapshots.
		 */
		return ext4_snapshot_read_block_bitmap(inode->i_sb,
						       block_group, bh_result);
	} else if (is_bitmap == EXCLUDE_BITMAP) {
		/* return unmapped buffer to zero out page */
		cancel_buffer_tracked_read(bh_result);
		/* cancel_buffer_tracked_read() clears mapped flag */
		snapshot_debug(2, "zeroing snapshot exclude bitmap #%u\n",
			       block_group);
		return 0;
	}

#ifdef CONFIG_EXT4_DEBUG
	snapshot_debug(3, "started tracked read: block = [%llu/%llu]\n",
		       SNAPSHOT_BLOCK_TUPLE(bh_result->b_blocknr));
	if (snapshot_enable_test[SNAPTEST_READ]) {
		err = ext4_snapshot_get_blockdev_access(inode->i_sb,
						    bh_result);
		if (err) {
			/* read through access denied */
			cancel_buffer_tracked_read(bh_result);
			return err;
		}
		/* sleep 1 tunable delay unit */
		snapshot_test_delay(SNAPTEST_READ);
	}
#endif
	return 0;
}

int ext4_snapshot_readpage(struct file *file, struct page *page)
{
	/* do read I/O with buffer heads to enable tracked reads */
	return ext4_read_full_page(page, ext4_snapshot_get_block);
}
