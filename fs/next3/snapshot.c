/*
 * linux/fs/next3/snapshot.c
 *
 * Written by Amir Goldstein <amir@ctera.com>, 2008
 *
 * Copyright (C) 2008-2010 CTERA Networks
 *
 * This file is part of the Linux kernel and is made available under
 * the terms of the GNU General Public License, version 2, or at your
 * option, any later version, incorporated herein by reference.
 *
 * Next3 snapshots core functions.
 */

#include <linux/quotaops.h>
#include "snapshot.h"

/*
 * helper functions
 */

/*
 * use 'mask' to clear exclude bitmap bits from block bitmap
 * when creating COW bitmap
 */
static inline void
__next3_snapshot_copy_bitmap(char *dst, const char *src, const char *mask)
{
	const u32 *ps = (const u32 *)src, *pm = (const u32 *)mask;
	u32 *pd = (u32 *)dst;
	int i;

	for (i = 0; i < SNAPSHOT_ADDR_PER_BLOCK; i++)
		*pd++ = *ps++ & ~*pm++;
}

/*
 * copy 'data' to (locked) snapshot buffer 'sbh'
 * 'mask' data before copying it to snapshot buffer
 */
static inline void
__next3_snapshot_copy_buffer(struct buffer_head *sbh,
		const char *data, const char *mask)
{
	if (mask)
		__next3_snapshot_copy_bitmap(sbh->b_data, data, mask);
	else
		memcpy(sbh->b_data, data, SNAPSHOT_BLOCK_SIZE);
	set_buffer_uptodate(sbh);
}

/*
 * next3_snapshot_complete_cow()
 * unlock a newly COWed snapshot buffer and complete the COW operation.
 * optinally, sync the buffer to disk or add it to the current transaction
 * as dirty data.
 */
static inline int
next3_snapshot_complete_cow(handle_t *handle,
		struct buffer_head *sbh, struct buffer_head *bh, int sync)
{
	int err = 0;
#ifdef CONFIG_NEXT3_FS_SNAPSHOT_RACE_READ
	SNAPSHOT_DEBUG_ONCE;
#endif

#ifdef CONFIG_NEXT3_FS_SNAPSHOT_RACE_READ
	/* wait for completion of tracked reads before completing COW */
	while (bh && buffer_tracked_readers_count(bh) > 0) {
		snapshot_debug_once(2, "waiting for tracked reads: "
			    "block = [%lld/%lld], "
			    "tracked_readers_count = %d...\n",
			    SNAPSHOT_BLOCK_GROUP_OFFSET(bh->b_blocknr),
			    SNAPSHOT_BLOCK_GROUP(bh->b_blocknr),
			    buffer_tracked_readers_count(bh));
		/* can happen once per block/snapshot - just keep trying */
		yield();
	}
#endif
	unlock_buffer(sbh);

	if (handle)
		err = next3_journal_dirty_data(handle, sbh);
	mark_buffer_dirty(sbh);
	if (sync)
		sync_dirty_buffer(sbh);

#ifdef CONFIG_NEXT3_FS_SNAPSHOT_RACE_COW
	/* COW operetion is complete */
	next3_snapshot_cancel_pending_cow(sbh);
#endif
	return err;
}

/*
 * next3_snapshot_copy_buffer_cow()
 * helper function for next3_snapshot_test_and_cow()
 * copy COWed buffer to new allocated (locked) snapshot buffer
 * add complete the COW operation
 */
static inline int
next3_snapshot_copy_buffer_cow(handle_t *handle,
				   struct buffer_head *sbh,
				   struct buffer_head *bh)
{
	__next3_snapshot_copy_buffer(sbh, bh->b_data, NULL);
	return next3_snapshot_complete_cow(handle, sbh, bh, 0);
}

/*
 * next3_snapshot_copy_buffer()
 * helper function for next3_snapshot_take()
 * used for initializing pre-allocated snapshot blocks
 * copy buffer to snapshot buffer and mark it dirty
 * 'mask' buffer before copying to snapshot.
 */
int next3_snapshot_copy_buffer(struct buffer_head *sbh,
		struct buffer_head *bh, const char *mask)
{
	lock_buffer(sbh);
	__next3_snapshot_copy_buffer(sbh, bh->b_data, mask);
	unlock_buffer(sbh);
	mark_buffer_dirty(sbh);
	return 0;
}

#ifdef CONFIG_NEXT3_FS_SNAPSHOT_EXCLUDE_FILES
/*
 * next3_snapshot_zero_buffer()
 * helper function for next3_snapshot_test_and_cow()
 * reset snapshot data buffer to zero and
 * add to current transaction (as dirty data)
 * 'blk' is the logical snapshot block number
 * 'blocknr' is the physical block number
 */
static int
next3_snapshot_zero_buffer(handle_t *handle, struct inode *inode,
		next3_snapblk_t blk, next3_fsblk_t blocknr)
{
	int err;
	struct buffer_head *sbh;
	sbh = sb_getblk(inode->i_sb, blocknr);
	if (!sbh)
		return -EIO;

	snapshot_debug(3, "zeroing snapshot block [%lld/%lld] = [%lu/%lu]\n",
			SNAPSHOT_BLOCK_GROUP_OFFSET(blk),
			SNAPSHOT_BLOCK_GROUP(blk),
			SNAPSHOT_BLOCK_GROUP_OFFSET(blocknr),
			SNAPSHOT_BLOCK_GROUP(blocknr));

	lock_buffer(sbh);
	memset(sbh->b_data, 0, SNAPSHOT_BLOCK_SIZE);
	set_buffer_uptodate(sbh);
	unlock_buffer(sbh);
	err = next3_journal_dirty_data(handle, sbh);
	mark_buffer_dirty(sbh);
	brelse(sbh);
	return err;
}
#endif

#define snapshot_debug_hl(n, f, a...)	snapshot_debug_l(n, handle ? 	\
						 handle->h_level : 0, f, ## a)

/*
 * next3_snapshot_get_inode_access() - called from next3_get_blocks_handle()
 * on snapshot file access.
 * return value <0 indicates access not granted
 * return value 0 indicates normal inode access
 * return value 1 indicates snapshot inode read through access
 * in which case 'prev_snapshot' is pointed to the previous snapshot
 * or set to NULL to indicate read through to block device.
 */
int next3_snapshot_get_inode_access(handle_t *handle, struct inode *inode,
		next3_fsblk_t iblock, int count, int cmd,
		struct inode **prev_snapshot)
{
	struct next3_inode_info *ei = NEXT3_I(inode);
#ifdef CONFIG_NEXT3_FS_SNAPSHOT_LIST_PEEP
	struct list_head *prev;
#endif
#ifdef CONFIG_NEXT3_FS_SNAPSHOT_DEBUG
	next3_fsblk_t block = SNAPSHOT_BLOCK(iblock);
	unsigned long block_group = (iblock < SNAPSHOT_BLOCK_OFFSET ? -1 :
			SNAPSHOT_BLOCK_GROUP(block));
	next3_grpblk_t blk = (iblock < SNAPSHOT_BLOCK_OFFSET ? iblock :
			SNAPSHOT_BLOCK_GROUP_OFFSET(block));
	snapshot_debug_hl(4, "snapshot (%u) get_blocks [%d/%lu] count=%d "
			  "cmd=%d\n", inode->i_generation, blk, block_group,
			  count, cmd);
#endif
	if (iblock < SNAPSHOT_META_BLOCKS) {
		/*
		 * Snapshot meta blocks access: h_level > 0 indicates this
		 * is snapshot_map_blocks recursive call
		 */
		if (handle && handle->h_level > 0) {
			snapshot_debug(1, "snapshot (%u) meta block (%d)"
					" - recursive access denied!\n",
					inode->i_generation, (int)iblock);
			return -EPERM;
		}
	} else if (iblock < SNAPSHOT_BLOCK_OFFSET) {
		/* snapshot reserved blocks */
		if (cmd != SNAPSHOT_READ) {
			snapshot_debug(1, "snapshot (%u) reserved block (%d)"
					" - %s access denied!\n",
					inode->i_generation, (int)iblock,
					snapshot_cmd_str(cmd));
			return -EPERM;
		}
	} else if (ei->i_flags & NEXT3_SNAPFILE_TAKE_FL) {
		/* only allocating blocks is allowed during snapshot take */
		if (cmd != SNAPSHOT_READ && cmd != SNAPSHOT_WRITE) {
			snapshot_debug(1, "snapshot (%u) during 'take'"
				       " - %s access denied!\n",
				       inode->i_generation,
				       snapshot_cmd_str(cmd));
			return -EPERM;
		}
	} else if (cmd == SNAPSHOT_WRITE) {
		/* snapshot image write access */
		snapshot_debug(1, "snapshot (%u) is read-only"
				" - write access denied!\n",
				inode->i_generation);
		return -EPERM;
	} else if (cmd == SNAPSHOT_READ) {
		/*
		 * Snapshot image read access: no handle or h_level == 0
		 * indicates this is a direct user read
		 */
		if (!handle || handle->h_level == 0)
			goto read_through;
	}
	/* normal inode access */
	return 0;

read_through:
	*prev_snapshot = NULL;
#ifdef CONFIG_NEXT3_FS_SNAPSHOT_LIST_PEEP
	/*
	 * Snapshot list add/delete is protected by lock_super() and we
	 * don't take lock_super() here.  However, since only
	 * old/unused/disabled snapshots are deleted, then we cannot be
	 * affected by deletes here.  If we are following the list
	 * during snapshot take, then we have 3 cases:
	 *
	 * 1. we got here before it was added to list - no problem;
	 * 2. we got here after it was added to list and after it was
	 *    activated - no problem;
	 * 3. we got here after it was added to list and before it was
	 *    activated - we skip it.
	 */
	prev = ei->i_orphan.prev;
	if (list_empty(prev)) {
		/* not in snapshots list */
		return -EIO;
	} else if (prev == &NEXT3_SB(inode->i_sb)->s_snapshot_list) {
		/* last snapshot - read through to block device */
		if (!next3_snapshot_is_active(inode))
			return -EIO;
	} else {
		/* non last snapshot - read through to prev snapshot */
		ei = list_entry(prev, struct next3_inode_info, i_orphan);
		if (!(ei->i_flags & NEXT3_SNAPFILE_TAKE_FL))
			/* skip over snapshot during take */
			*prev_snapshot = &ei->vfs_inode;
	}
	/* read through access */
	return 1;
#endif
}

/*
 * next3_snapshot_map_blocks() - helper function for
 * next3_snapshot_test_and_cow().
 * Also used by next3_snapshot_create() to pre-allocate snapshot blocks.
 * Check for mapped block (cmd == SNAPSHOT_READ) or map new blocks to
 * snapshot file.
 */
int next3_snapshot_map_blocks(handle_t *handle, struct inode *inode,
			      next3_snapblk_t block, unsigned long maxblocks,
			      next3_fsblk_t *mapped, int cmd)
{
	struct buffer_head dummy;
	int err;

	dummy.b_state = 0;
	dummy.b_blocknr = -1000;
	err = next3_get_blocks_handle(handle, inode, SNAPSHOT_IBLOCK(block),
				      maxblocks, &dummy, cmd);
	/*
	 * next3_get_blocks_handle() returns number of blocks
	 * mapped. 0 in case of a HOLE.
	 */
	if (err <= 0)
		dummy.b_blocknr = 0;
	else if (mapped)
		*mapped = dummy.b_blocknr;

	snapshot_debug_hl(4, "snapshot (%u) map_blocks "
			  "[%lld/%lld] = [%lld/%lld] "
			  "cmd=%d, maxblocks=%lu, mapped=%d\n",
			  inode->i_generation,
			  SNAPSHOT_BLOCK_GROUP_OFFSET(block),
			  SNAPSHOT_BLOCK_GROUP(block),
			  SNAPSHOT_BLOCK_GROUP_OFFSET(dummy.b_blocknr),
			  SNAPSHOT_BLOCK_GROUP(dummy.b_blocknr),
			  cmd, maxblocks, err);
	return err;
}

/*
 * COW bitmap functions
 */

/*
 * next3_snapshot_init_cow_bitmap() init a new allocated (locked) COW bitmap
 * buffer on first time block group access after snapshot take.
 * COW bitmap is created by masking the block bitmap with exclude bitmap.
 */
static int
next3_snapshot_init_cow_bitmap(struct super_block *sb,
		unsigned int block_group, struct buffer_head *cow_bh)
{
	struct buffer_head *bitmap_bh;
#ifdef CONFIG_NEXT3_FS_SNAPSHOT_EXCLUDE_BITMAP
	struct buffer_head *exclude_bitmap_bh = NULL;
#endif
	const char *data, *mask = NULL;
	struct journal_head *jh;

	bitmap_bh = read_block_bitmap(sb, block_group);
	if (!bitmap_bh)
		return -EIO;

	data = bitmap_bh->b_data;
#ifdef CONFIG_NEXT3_FS_SNAPSHOT_EXCLUDE_BITMAP
	exclude_bitmap_bh = read_exclude_bitmap(sb, block_group);
	if (exclude_bitmap_bh)
		/* mask block bitmap with exclude bitmap */
		mask = exclude_bitmap_bh->b_data;
#endif
	/*
	 * Another COWing task may be changing this block bitmap
	 * (allocating active snapshot blocks) while we are trying
	 * to copy it.  Copying committed_data will keep us
	 * protected from these changes.  At this point we are
	 * guaranteed that the only difference between block bitmap
	 * and commited_data are the new active snapshot blocks,
	 * because before allocating/freeing any other blocks a task
	 * must first get_undo_access() and get here.
	 */
	jbd_lock_bh_journal_head(bitmap_bh);
	jbd_lock_bh_state(bitmap_bh);
	jh = bh2jh(bitmap_bh);
	if (jh && jh->b_committed_data)
		data = jh->b_committed_data;

	__next3_snapshot_copy_buffer(cow_bh, data, mask);

	jbd_unlock_bh_state(bitmap_bh);
	jbd_unlock_bh_journal_head(bitmap_bh);

#ifdef CONFIG_NEXT3_FS_SNAPSHOT_EXCLUDE_BITMAP
	brelse(exclude_bitmap_bh);
#endif
	brelse(bitmap_bh);
	return 0;
}

/*
 * next3_snapshot_read_block_bitmap()
 * helper function for next3_snapshot_get_block()
 * used for fixing the block bitmap buffer when
 * reading through to block device.
 */
int next3_snapshot_read_block_bitmap(struct super_block *sb,
		unsigned int block_group, struct buffer_head *bitmap_bh)
{
	int err;

	lock_buffer(bitmap_bh);
	err = next3_snapshot_init_cow_bitmap(sb, block_group, bitmap_bh);
	unlock_buffer(bitmap_bh);
	return err;
}

/*
 * next3_snapshot_read_cow_bitmap() reads the COW bitmap for a given
 * block_group.  Creates the COW bitmap on first time after snapshot take.
 * COW bitmap cache is non-persistent, so no need to mark the group desc
 * block dirty.
 *
 * Return buffer_head on success or NULL in case of failure.
 */
static struct buffer_head *
next3_snapshot_read_cow_bitmap(handle_t *handle, struct inode *snapshot,
			       unsigned int block_group)
{
	struct super_block *sb = snapshot->i_sb;
	struct next3_sb_info *sbi = NEXT3_SB(sb);
	struct next3_group_desc *desc;
	struct buffer_head *cow_bh;
	next3_fsblk_t bitmap_blk;
	next3_fsblk_t cow_bitmap_blk;
	int err = 0;
#ifdef CONFIG_NEXT3_FS_SNAPSHOT_RACE_BITMAP
	SNAPSHOT_DEBUG_ONCE;
#endif

	desc = next3_get_group_desc(sb, block_group, NULL);
	if (!desc)
		return NULL;

	bitmap_blk = le32_to_cpu(desc->bg_block_bitmap);
	spin_lock(sb_bgl_lock(sbi, block_group));
	cow_bitmap_blk = le32_to_cpu(desc->bg_cow_bitmap);
	spin_unlock(sb_bgl_lock(sbi, block_group));

#ifdef CONFIG_NEXT3_FS_SNAPSHOT_RACE_BITMAP
	/* handle concurrect COW bitmap operations */
	while (cow_bitmap_blk == 0 || cow_bitmap_blk == bitmap_blk) {
		spin_lock(sb_bgl_lock(sbi, block_group));
		cow_bitmap_blk = le32_to_cpu(desc->bg_cow_bitmap);
		if (cow_bitmap_blk == 0)
			/* mark pending COW of bitmap block */
			desc->bg_cow_bitmap = bitmap_blk;
		spin_unlock(sb_bgl_lock(sbi, block_group));

		if (cow_bitmap_blk == 0) {
			snapshot_debug(3, "COWing bitmap #%u of snapshot "
				       "(%u)...\n", block_group,
				       snapshot->i_generation);
			/* sleep 1 tunable delay unit */
			snapshot_test_delay(SNAPTEST_BITMAP);
			break;
		}
		if (cow_bitmap_blk == bitmap_blk) {
			/* wait for another task to COW bitmap block */
			snapshot_debug_once(2, "waiting for pending cow "
					    "bitmap #%d...\n", block_group);
			/*
			 * can happen once per block_group/snapshot - just
			 * keep trying
			 */
			yield();
		}
	}
#endif

	if (cow_bitmap_blk)
		return sb_bread(sb, cow_bitmap_blk);

	/*
	 * Try to read cow bitmap block from snapshot file.  If COW bitmap
	 * is not yet allocated, create the new COW bitmap block.
	 */
	cow_bh = next3_bread(handle, snapshot, SNAPSHOT_IBLOCK(bitmap_blk),
				SNAPSHOT_READ, &err);
	if (cow_bh)
		goto out;

	/* allocate snapshot block for COW bitmap */
	cow_bh = next3_getblk(handle, snapshot, SNAPSHOT_IBLOCK(bitmap_blk),
				SNAPSHOT_BITMAP, &err);
	if (!cow_bh || err < 0)
		goto out;
	if (!err) {
		/*
		 * err should be 1 to indicate new allocated (locked) buffer.
		 * if err is 0, it means that someone mapped this block
		 * before us, while we are updating the COW bitmap cache.
		 * the pending COW bitmap code should prevent that.
		 */
		WARN_ON(1);
		err = -EIO;
		goto out;
	}

	err = next3_snapshot_init_cow_bitmap(sb, block_group, cow_bh);
	if (err)
		goto out;
	/*
	 * complete pending COW operation. no need to wait for tracked reads
	 * of block bitmap, because it is copied directly to page buffer by
	 * next3_snapshot_read_block_bitmap()
	 */
	err = next3_snapshot_complete_cow(handle, cow_bh, NULL, 1);
	if (err)
		goto out;

	snapshot_debug(3, "COW bitmap #%u of snapshot (%u) "
			"mapped to block [%lld/%lld]\n",
			block_group, snapshot->i_generation,
			SNAPSHOT_BLOCK_GROUP_OFFSET(cow_bh->b_blocknr),
			SNAPSHOT_BLOCK_GROUP(cow_bh->b_blocknr));
#ifdef CONFIG_NEXT3_FS_SNAPSHOT_JOURNAL_TRACE
	handle->h_cow_bitmaps++;
#endif

out:
	spin_lock(sb_bgl_lock(sbi, block_group));
	if (!err && cow_bh) {
		/* update cow bitmap cache with snapshot cow bitmap block */
		desc->bg_cow_bitmap = cow_bh->b_blocknr;
	} else {
		/* reset cow bitmap cache */
		desc->bg_cow_bitmap = 0;
		brelse(cow_bh);
		cow_bh = NULL;
	}
	spin_unlock(sb_bgl_lock(sbi, block_group));

	if (!cow_bh)
		snapshot_debug(1, "failed to read COW bitmap %u of snapshot "
				"(%u)\n", block_group, snapshot->i_generation);
	return cow_bh;
}

/*
 * next3_snapshot_test_cow_bitmap() tests if the block is in use by snapshot
 */
static int
next3_snapshot_test_cow_bitmap(handle_t *handle, struct inode *snapshot,
			       next3_fsblk_t block)
{
	struct buffer_head *cow_bh;
	unsigned long block_group = SNAPSHOT_BLOCK_GROUP(block);
	next3_grpblk_t bit = SNAPSHOT_BLOCK_GROUP_OFFSET(block);
	int snapshot_blocks = SNAPSHOT_BLOCKS(snapshot);

	/* check if block is in snapshot range (maybe fs was resized) */
	if (block >= snapshot_blocks) {
		snapshot_debug_hl(4, "snapshot (%u) test COW bitmap: block "
				  "[%d/%lu] out of range (snapshot_blocks=%d)"
				  "\n", snapshot->i_generation, bit,
				  block_group, snapshot_blocks);
		return 0;
	}

	cow_bh = next3_snapshot_read_cow_bitmap(handle, snapshot, block_group);
	if (!cow_bh)
		return -1;
	/*
	 * if the bit is set in the COW bitmap,
	 * then this block is in use by some snapshot.
	 */
	return next3_test_bit(bit, cow_bh->b_data) ? 1 : 0;
}

#ifdef CONFIG_NEXT3_FS_SNAPSHOT_EXCLUDE_BITMAP
/*
 * next3_snapshot_exclude_blocks() marks blocks in exclude bitmap
 */
static int
next3_snapshot_exclude_blocks(handle_t *handle, struct inode *snapshot,
			      next3_fsblk_t block, int count)
{
	struct buffer_head *exclude_bitmap_bh = NULL;
	unsigned long block_group = SNAPSHOT_BLOCK_GROUP(block);
	next3_grpblk_t bit = SNAPSHOT_BLOCK_GROUP_OFFSET(block);
	struct super_block *sb = snapshot->i_sb;
	int err = 0, n = 0, excluded = 0;

	exclude_bitmap_bh = read_exclude_bitmap(sb, block_group);
	if (!exclude_bitmap_bh)
		return 0;

	err = next3_journal_get_write_access(handle, exclude_bitmap_bh);
	if (err)
		return err;

	while (count > 0 && bit < SNAPSHOT_BLOCKS_PER_GROUP) {
		if (!next3_set_bit_atomic(sb_bgl_lock(NEXT3_SB(sb),
						block_group),
					bit, exclude_bitmap_bh->b_data)) {
			n++;
		} else if (n) {
			snapshot_debug(2, "excluded blocks: [%d-%d/%ld]\n",
					bit-n, bit-1, block_group);
			excluded += n;
			n = 0;
		}
		bit++;
		count--;
	}

	if (n) {
		snapshot_debug(2, "excluded blocks: [%d-%d/%ld]\n",
				bit-n, bit-1, block_group);
		excluded += n;
	}

	if (excluded)
		err = next3_journal_dirty_metadata(handle, exclude_bitmap_bh);

	brelse(exclude_bitmap_bh);
	return err ? err : excluded;
}
#endif

/*
 * COW functions
 */

#ifdef CONFIG_NEXT3_FS_SNAPSHOT_DEBUG
static void
__next3_snapshot_trace_cow(handle_t *handle, struct inode *inode,
			   struct buffer_head *bh, next3_fsblk_t block,
			   int code, const char *fn)
{
	unsigned long inode_group = 0;
	next3_grpblk_t inode_offset = 0;

	if (inode) {
		inode_group = (inode->i_ino - 1) /
			NEXT3_INODES_PER_GROUP(inode->i_sb);
		inode_offset = (inode->i_ino - 1) %
			NEXT3_INODES_PER_GROUP(inode->i_sb);
	}
	snapshot_debug_hl(4, "get_%s_access(i:%d/%ld, b:%lu/%lu)"
			" h_ref=%d, h_level=%d, code=%d\n",
			fn, inode_offset, inode_group,
			SNAPSHOT_BLOCK_GROUP_OFFSET(block),
			SNAPSHOT_BLOCK_GROUP(block),
			handle->h_ref, handle->h_level, code);
}

#define next3_snapshot_trace_cow(handle, inode, bh, block, code, fn) \
	if (snapshot_enable_debug >= 4)					\
		__next3_snapshot_trace_cow(handle, inode, bh, block, code, fn)
#else
#define next3_snapshot_trace_cow(handle, inode, bh, block, code, fn)
#endif

/*
 * next3_snapshot_test_and_cow() tests if the block should be cowed
 * if the 'cmd' is SNAPSHOT_COPY, try to copy the block to the snapshot
 * if the 'cmd' is SNAPSHOT_MOVE, try to move the block to the snapshot
 * if the 'cmd' is SNAPSHOT_CLEAR, try to mark the block excluded from snapshot
 *
 * if SNAPSHOT_OK or SNAPSHOT_MOVED are returned,
 * then the block may be safely written over.
 * if SNAPSHOT_COW is returned,
 * next3_snapshot_cow() must be called before writing to the buffer
 */
static int
next3_snapshot_test_and_cow(handle_t *handle, struct inode *inode,
			    struct buffer_head *bh, next3_fsblk_t block,
			    int *pcount, int cmd, const char *fn)
{
#warning function too long, over 300 LoC
	struct super_block *sb = handle->h_transaction->t_journal->j_private;
	struct inode *snapshot = next3_snapshot_get_active(sb);
	struct buffer_head *sbh = NULL;
	struct journal_head *jh;
	next3_fsblk_t blk = 0;
	int err = -EIO;
	int count = pcount ? *pcount : 1;
	int clear = 0;

	next3_snapshot_trace_cow(handle, inode, bh, block, cmd, fn);
	snapshot_debug_hl(4, "{\n");

	if (handle->h_level++ >= SNAPSHOT_MAX_RECURSION_LEVEL) {
		snapshot_debug_hl(1,
			  "COW error: max recursion level exceeded!\n");
		goto skip_cow;
	}

	err = SNAPSHOT_OK;
	if (!snapshot)
		goto skip_cow;

	/* avoid recursion on active snapshot file updates */
	if (handle->h_level > 1 ||
#ifdef CONFIG_NEXT3_FS_SNAPSHOT_EXCLUDE_INODE
		next3_snapshot_exclude_inode(inode) ||
#endif
		(inode == snapshot && cmd != SNAPSHOT_CLEAR)) {
		snapshot_debug_hl(4, "active snapshot file update - "
				  "skip block cow!\n");
		goto skip_cow;
	}

#ifdef CONFIG_NEXT3_FS_SNAPSHOT_JOURNAL_CACHE
	/* first check the COW tid in the journal head */
	if (bh && buffer_jbd(bh)) {
		jbd_lock_bh_state(bh);
		jh = bh2jh(bh);
		if (jh && jh->b_cow_tid != handle->h_transaction->t_tid)
			jh = NULL;
		jbd_unlock_bh_state(bh);
		if (jh) {
			/*
			 * This is not the first time this block is being
			 * COWed in the running transaction, so we don't
			 * need to COW it again.
			 */
#ifdef CONFIG_NEXT3_FS_SNAPSHOT_JOURNAL_TRACE
			handle->h_cow_ok_jh++;
#endif
			goto skip_cow;
		}
	}
#endif

	if (!inode || cmd == SNAPSHOT_READ)
		/* skip excluded file test */
		goto test_cow;
	clear = next3_snapshot_excluded(inode);
	if (!clear)
		/* file is not excluded */
		goto test_cow;

	if (cmd < 0 || clear < 0) {
		/* excluded file block move/delete/clear access
		 * or ignored file block write access -
		 * mark block in exclude bitmap */
		snapshot_debug_hl(4, "file (%lu) excluded from snapshot - "
				"mark block (%lu) in exclude bitmap\n",
				inode->i_ino, block);
		cmd = SNAPSHOT_READ;
	} else {
#ifdef CONFIG_NEXT3_FS_SNAPSHOT_EXCLUDE_FILES
		/* excluded file block write access -
		 * COW and zero out snapshot block */
		snapshot_debug_hl(4, "file (%lu) excluded from snapshot - "
				"zero out block (%lu) in snapshot\n",
				inode->i_ino, block);
#else
		/* user excluded files are not supported */
		BUG_ON(clear > 0);
#endif
	}

test_cow:
#ifdef CONFIG_NEXT3_FS_SNAPSHOT_JOURNAL_CREDITS
	if (!NEXT3_SNAPSHOT_HAS_TRANS_BLOCKS(handle, 1)) {
		snapshot_debug_hl(1, "COW warning: insuffiecient buffer/user "
				  "credits (%d/%d) for COW operation?\n",
				  handle->h_buffer_credits,
				  handle->h_user_credits);
	}
#endif

	/* first get the COW bitmap and test if the block needs to be COWed */
	err = next3_snapshot_test_cow_bitmap(handle, snapshot, block);
#ifdef CONFIG_NEXT3_FS_SNAPSHOT_JOURNAL_TRACE
	if (err == 0)
		handle->h_cow_ok_clear++;
#endif
	if (err && clear < 0) {
		/* ignore COW bitmap test result for ignored file blocks */
#ifdef CONFIG_NEXT3_FS_SNAPSHOT_EXCLUDE_BITMAP
		snapshot_debug_hl(1, "warning: file (%lu) is excluded from "
				  "snapshot but block (%lu) is set in "
				  "COW bitmap!\n", inode->i_ino, block);
#endif
		err = 0;
	}
	if (err <= 0)
		goto out;

	/* then test if snapshot already has a private copy of the block */
	err = next3_snapshot_map_blocks(handle, snapshot, block, 1, &blk,
					SNAPSHOT_READ);
	if (err < 0)
		goto out;
	if (err > 0) {
#ifdef CONFIG_NEXT3_FS_SNAPSHOT_JOURNAL_TRACE
		handle->h_cow_ok_mapped++;
#endif
		sbh = sb_find_get_block(sb, blk);
		goto test_pending_cow;
	}

	err = SNAPSHOT_COW;
	if (cmd == SNAPSHOT_READ)
		goto out;

#ifdef CONFIG_NEXT3_FS_SNAPSHOT_FILE_MOVE
	/* if move or delete were requested, try to map the block to snapshot */
	if (cmd == SNAPSHOT_MOVE) {
		if (inode == NULL) {
			/*
			 * freeing blocks of newly added block group - don't
			 * move them to snapshot
			 */
#ifdef CONFIG_NEXT3_FS_SNAPSHOT_JOURNAL_TRACE
			handle->h_cow_ok_clear++;
#endif
			err = 0;
			goto out;
		}
		/* move block from inode to snapshot */
		err = next3_snapshot_map_blocks(handle, snapshot, block,
						count, NULL, cmd);
		if (err) {
			if (err > 0) {
				count = err;
				if (pcount)
					*pcount = count;
				vfs_dq_free_block(inode, err);
				err = SNAPSHOT_MOVED;
#ifdef CONFIG_NEXT3_FS_SNAPSHOT_EXCLUDE_BITMAP
				/* set moved blocks in exclude bitmap */
				clear = -1;
#endif
#ifdef CONFIG_NEXT3_FS_SNAPSHOT_JOURNAL_TRACE
				handle->h_cow_moved++;
#endif
			}
			goto out;
		}
	}
#endif

	err = SNAPSHOT_COW;
	if (cmd != SNAPSHOT_COPY)
		goto out;

	err = -EIO;
	/* make sure we hold an uptodate source buffer */
	if (!bh || !buffer_mapped(bh) || bh->b_blocknr != block)
		goto out;
	if (!buffer_uptodate(bh)) {
		snapshot_debug(1, "warning: non uptodate buffer (%lld)"
				" needs to be copied to active snapshot!\n",
				bh->b_blocknr);
		ll_rw_block(READ, 1, &bh);
		wait_on_buffer(bh);
		if (!buffer_uptodate(bh))
			goto out;
	}

	/* next try to allocate snapshot block to make a backup copy */
	sbh = next3_getblk(handle, snapshot, SNAPSHOT_IBLOCK(block),
			   SNAPSHOT_COPY, &err);
	if (!sbh || err < 0)
		goto out;

	if (err) {
		/*
		 * locked buffer - copy block data to snapshot and unlock
		 * buffer
		 */
#ifdef CONFIG_NEXT3_FS_SNAPSHOT_RACE_COW
		snapshot_debug(3, "COWing block [%lld/%lld] of snapshot "
			       "(%u)...\n",
				SNAPSHOT_BLOCK_GROUP_OFFSET(bh->b_blocknr),
				SNAPSHOT_BLOCK_GROUP(bh->b_blocknr),
				snapshot->i_generation);
		/* sleep 1 tunable delay unit */
		snapshot_test_delay(SNAPTEST_COW);
#endif
		err = next3_snapshot_copy_buffer_cow(handle, sbh, bh);
		if (err)
			goto out;
		snapshot_debug(3, "block [%lld/%lld] of snapshot (%u) "
				"mapped to block [%lld/%lld]\n",
				SNAPSHOT_BLOCK_GROUP_OFFSET(bh->b_blocknr),
				SNAPSHOT_BLOCK_GROUP(bh->b_blocknr),
				snapshot->i_generation,
				SNAPSHOT_BLOCK_GROUP_OFFSET(sbh->b_blocknr),
				SNAPSHOT_BLOCK_GROUP(sbh->b_blocknr));
#ifdef CONFIG_NEXT3_FS_SNAPSHOT_JOURNAL_TRACE
		handle->h_cow_copied++;
#endif
	}
#ifdef CONFIG_NEXT3_FS_SNAPSHOT_JOURNAL_TRACE
	else
		handle->h_cow_ok_mapped++;
#endif

test_pending_cow:
#ifdef CONFIG_NEXT3_FS_SNAPSHOT_RACE_COW
	if (sbh)
		/* wait for pending COW to complete */
		next3_snapshot_test_pending_cow(sbh, block);
#endif
#ifdef CONFIG_NEXT3_FS_SNAPSHOT_EXCLUDE_FILES
	if (sbh && !blk)
		blk = sbh->b_blocknr;
	if (clear && blk) {
		/* zero out snapshot block data */
		err = next3_snapshot_zero_buffer(handle, snapshot,
						      bh->b_blocknr, blk);
		if (err)
			goto out;
	}
#endif

	err = SNAPSHOT_OK;
out:
#ifdef CONFIG_NEXT3_FS_SNAPSHOT_JOURNAL_CACHE
	if (bh && buffer_jbd(bh)) {
		jbd_lock_bh_state(bh);
		jh = bh2jh(bh);
		if (jh && jh->b_cow_tid != handle->h_transaction->t_tid) {
			/*
			 * this is the first time this block is being COWed
			 * in the runnning transaction.
			 * update the COW tid in the journal head
			 * to mark that this block doesn't need to be COWed.
			 */
			if (!err)
				jh->b_cow_tid = handle->h_transaction->t_tid;
		}
		jbd_unlock_bh_state(bh);
	}
#endif

#ifdef CONFIG_NEXT3_FS_SNAPSHOT_EXCLUDE_BITMAP
	if (clear < 0) {
		/* mark blocks in exclude bitmap */
		count = next3_snapshot_exclude_blocks(handle, snapshot,
						      block, count);
		if (count < 0)
			err = count;
#ifdef CONFIG_NEXT3_FS_SNAPSHOT_JOURNAL_TRACE
		else if (count > 0)
			handle->h_cow_cleared++;
#endif
	}
#endif

	brelse(sbh);
skip_cow:
	handle->h_level--;
	snapshot_debug_hl(4, "} = %d\n", err);
	snapshot_debug_hl(4, ".\n");

	return err;
}

/*
 * tests if the block should be cowed
 */
#define next3_snapshot_test_cow(handle, bh, fn)				\
	next3_snapshot_test_and_cow(handle, NULL, bh, bh->b_blocknr, NULL, \
				    SNAPSHOT_READ, fn)
/*
 * tests if the metadata block should be cowed
 * and in case it does, tries to copy the block to the snapshot
 */
#define next3_snapshot_cow(handle, inode, bh, fn)			\
	next3_snapshot_test_and_cow(handle, inode, bh, bh->b_blocknr, NULL, \
				    SNAPSHOT_COPY, fn)

#ifdef CONFIG_NEXT3_FS_SNAPSHOT_FILE_MOVE
/*
 * tests if the data block should be cowed
 * and in case it does, tries to move the block to the snapshot
 */
#define next3_snapshot_mow(handle, inode, block, num, fn)	     \
	next3_snapshot_test_and_cow(handle, inode, NULL, block, num, \
				    SNAPSHOT_MOVE, fn)
#endif
/*
 * tests if the block should be cowed
 * and in case it does, tries to clear the block from the COW bitmap
 */
#define next3_snapshot_clear(handle, inode, block, num, fn)	     \
	next3_snapshot_test_and_cow(handle, inode, NULL, block, num, \
				    SNAPSHOT_CLEAR, fn)

/*
 * Block access functions
 */

/*
 * get_write_access() is called before writing to a metadata block
 * if inode is not NULL, then this is an inode's indirect block
 * otherwise, this is a file system global metadata block
 */
int next3_snapshot_get_write_access(handle_t *handle, struct inode *inode,
				    struct buffer_head *bh)
{
	return next3_snapshot_cow(handle, inode, bh, "write");
}

/*
 * get_undo_access() is called for group bitmap block from:
 * 1. next3_free_blocks_sb_inode() before deleting blocks
 * 2. next3_new_blocks() before allocating blocks
 */
int next3_snapshot_get_undo_access(handle_t *handle, struct buffer_head *bh)
{
	int ret = next3_snapshot_test_cow(handle, bh, "undo");

	if (ret != SNAPSHOT_COW)
		return ret;
	/*
	 * We shouldn't get here if everything works properly because undo
	 * access is only requested for block bitmaps which should be COW'ed
	 * in next3_snapshot_test_cow_bitmap()
	 */
	snapshot_debug(1, "warning: block bitmap [%lld/%lld]"
		       " needs to be COWed?!\n",
		       SNAPSHOT_BLOCK_GROUP_OFFSET(bh->b_blocknr),
		       SNAPSHOT_BLOCK_GROUP(bh->b_blocknr));
	return ret;
}

/*
 * get_create_access() is called after allocating a new metadata block
 */
int next3_snapshot_get_create_access(handle_t *handle, struct buffer_head *bh)
{
	int ret;

	ret = next3_snapshot_test_cow(handle, bh, "create");
	if (ret != SNAPSHOT_COW)
		return ret;
#ifdef CONFIG_NEXT3_FS_SNAPSHOT_FILE_MOVE
	/*
	 * We shouldn't get here if get_delete_access() was called for all
	 * deleted blocks.  However, we could definetly get here if fsck was
	 * run and if it had freed some blocks, naturally, without moving
	 * them to snapshot.
	 */
	snapshot_debug(1, "warning: new allocated block (%lld)"
		       " needs to be COWed?!\n", bh->b_blocknr);
#else
	/*
	 * Never mind why we got here, we have to try to COW the new
	 * allocated block before it is overwritten.  If allocation is for
	 * active snapshot itself (h_level > 0), there is nothing graceful
	 * we can do, so issue an fs error.
	 */
	if (handle->h_level > 0)
		return ret;
	/*
	 * Buffer comes here locked and should return locked but we must
	 * unlock it, because it may not be up-to-date, so it may be read
	 * from disk before it is being COWed.
	 */
	unlock_buffer(bh);
	ret = next3_snapshot_cow(handle, NULL, bh, "create");
	lock_buffer(bh);
#endif
	return ret;

}

#ifdef CONFIG_NEXT3_FS_SNAPSHOT_FILE_MOVE
/*
 * get_move_access() is called from:
 * next3_get_branch_cow() before overwriting a data block
 *
 * if 0 is returned, the block may be safely overwritten
 * if N>0 is returned, then N blocks are used by the snapshot
 * and may not be overwritten (data should be moved)
 */
int next3_snapshot_get_move_access(handle_t *handle, struct inode *inode,
		next3_fsblk_t block, int count)
{
	int ret = next3_snapshot_mow(handle, inode, block, &count, "move");

	if (ret == SNAPSHOT_MOVED)
		ret = count;
	else if (ret > 0)
		ret = 0;
	return ret;
}

/*
 * get_delete_access() is called from:
 * next3_free_blocks_sb_inode() before deleting a block
 *
 * if 0 is returned, the block may be safely deleted
 * if N>0 is returned, then N blocks are used by the snapshot
 * and may not be deleted
 */
int next3_snapshot_get_delete_access(handle_t *handle, struct inode *inode,
		next3_fsblk_t block, int count)
{
	int ret = next3_snapshot_mow(handle, inode, block, &count, "delete");

	if (ret == SNAPSHOT_MOVED)
		ret = count;
	else if (ret > 0)
		ret = 0;
	return ret;
}
#endif

#ifdef CONFIG_NEXT3_FS_SNAPSHOT_FILE_EXCLUDE
/*
 * get_clear_access() is called from:
 * next3_clear_branch() for all blocks in a branch
 *
 * if 0 is returned, the block may be safely accessed
 * without being COWed to snapshot
 */
int next3_snapshot_get_clear_access(handle_t *handle, struct inode *inode,
				    next3_fsblk_t block, int count)
{
	return next3_snapshot_clear(handle, inode, block, &count, "clear");
}
#endif
