/*
 * linux/fs/next3/snapshot.c
 *
 * Copyright (C) 2008 Amir Goldor <amir73il@users.sf.net>
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

#ifdef CONFIG_NEXT3_FS_SNAPSHOT_EXCLUDE_BITMAP
/*
 * use 'mask' to clear exclude bitmap bits from block bitmap
 * when creating COW bitmap
 */
static inline void
next3_snapshot_copy_bitmap(char *dst, const char *src, const char *mask)
{
	const u32 *ps = (const u32 *)src, *pm = (const u32 *)mask;
	u32 *pd = (u32 *)dst;
	int i;
	for (i=0; i<SNAPSHOT_ADDR_PER_BLOCK; i++)
		*pd++ = *ps++ & ~*pm++;
}
#endif

/* 
 * next3_snapshot_copy_to_buffer()
 * copy data to locked snapshot buffer and unlock it
 * 'mask' data before copying to snapshot.
 */
static inline int
next3_snapshot_copy_to_buffer(handle_t *handle, struct buffer_head *sbh, 
		struct buffer_head *bh, const char *data, const char *mask)
{
	SNAPSHOT_DEBUG_ONCE;
	int err = 0;
	
#ifdef CONFIG_NEXT3_FS_SNAPSHOT_EXCLUDE_BITMAP
	if (mask)
		next3_snapshot_copy_bitmap(sbh->b_data, data, mask);
	else
#endif
		memcpy(sbh->b_data, data, SNAPSHOT_BLOCK_SIZE);
	set_buffer_uptodate(sbh);

#ifdef CONFIG_NEXT3_FS_SNAPSHOT_RACE_READ
	/* wait for completion of tracked reads before completing COW */
	while (buffer_tracked_readers_count(bh) > 0) {
		snapshot_debug_once(2, "waiting for tracked reads: block = [%lld/%lld]"
				", tracked_readers_count = %d...\n",
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
	if (!handle)
		sync_dirty_buffer(sbh);

#ifdef CONFIG_NEXT3_FS_SNAPSHOT_RACE_COW
	/* 
	 * clearing the new flag indicates that COW operation is complete.
	 * for as long as this snapshot should live, 
	 * this block and its buffer cache will never get dirty again.
	 */
	clear_buffer_new(sbh);
	/* 
	 * COW operetion is complete -
	 * so we no longer need to hold buffer in cache 
	 */
	put_bh(sbh);
#endif
	return err;
}

/* 
 * next3_snapshot_copy_to_buffer_sync()
 * helper function for next3_snapshot_read_cow_bitmap()
 * used for initializing snapshot COW bitmaps
 * copy data to snapshot buffer and sync it to disk
 * 'mask' buffer before copying to snapshot.
 */
static inline void 
next3_snapshot_copy_to_buffer_sync(struct buffer_head *sbh, 
		struct buffer_head *bh, const char *data, const char *mask)
{
	lock_buffer(sbh);
	next3_snapshot_copy_to_buffer(NULL, sbh, bh, data, mask);
}

/* 
 * next3_snapshot_copy_buffer_ordered()
 * helper function for next3_snapshot_test_and_cow() 
 * copy buffer to locked snapshot buffer and unlock it
 * add snapshot buffer to current transaction (as dirty data)
 */
static inline int
next3_snapshot_copy_buffer_ordered(handle_t *handle, 
		struct buffer_head *sbh, struct buffer_head *bh)
{
	return next3_snapshot_copy_to_buffer(handle, sbh, bh, bh->b_data, NULL);
}

/* 
 * next3_snapshot_copy_buffer_sync()
 * helper function for next3_snapshot_take()
 * used for initializing pre-allocated snapshot blocks
 * copy buffer to snapshot buffer and sync it to disk
 */
int next3_snapshot_copy_buffer_sync(struct buffer_head *sbh, 
									struct buffer_head *bh)
{
	/* 
	 * the path coming from snapshot_take()
	 * doesn't have elevated refcount on snaphshot buffer
	 * because these blocks are pre-allocated
	 */
	get_bh(sbh);
	next3_snapshot_copy_to_buffer_sync(sbh, bh, bh->b_data, NULL);
	return 0;
}

/* 
 * next3_snapshot_zero_data_buffer()
 * helper function for next3_snapshot_create() 
 * and for next3_snapshot_test_and_cow() 
 * reset snapshot data buffer to zero and
 * add to current transaction (as dirty data)
 * 'blk' is the logical snapshot block number
 * 'blocknr' is the physical block number
 */
int
next3_snapshot_zero_data_buffer(handle_t *handle, struct inode *inode, 
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


#define snapshot_debug_hl(n, f, a...)	\
		snapshot_debug_l(n, handle ? 	\
				handle->h_level : 0, f, ## a)


/* 
 * next3_snapshot_get_inode_access() - called from next3_get_blocks_handle() 
 * on snapshot file access.
 * return value <0 indicates access not granted
 * return value 0 indicates normal inode access
 * return value 1 indicates snapshot inode access (peep through)
 */
int next3_snapshot_get_inode_access(handle_t *handle, struct inode *inode,
		next3_fsblk_t iblock, int count, int cmd)
{
	struct next3_inode_info *ei = NEXT3_I(inode);
#ifdef CONFIG_NEXT3_FS_SNAPSHOT_DEBUG
	next3_fsblk_t block = SNAPSHOT_BLOCK(iblock);
	unsigned long block_group = (iblock < SNAPSHOT_BLOCK_OFFSET ? -1 : 
			SNAPSHOT_BLOCK_GROUP(block));
	next3_grpblk_t blk = (iblock < SNAPSHOT_BLOCK_OFFSET ? iblock : 
			SNAPSHOT_BLOCK_GROUP_OFFSET(block));
	snapshot_debug_hl(4, "snapshot (%u) get_blocks [%d/%lu] count=%d cmd=%d\n", 
			inode->i_generation, blk, block_group, count, cmd);
#endif
	if (iblock < SNAPSHOT_META_BLOCKS) {
		/* snapshot meta blocks access */
		/* h_level > 0 indicates this is snapshot_map_blocks recursive call */
		if (handle && handle->h_level > 0) {
			snapshot_debug(1, "snapshot (%u) meta block (%d)"
					" - recursive access denied!\n", 
					inode->i_generation, (int)iblock);
			return -EPERM;
		}
	} 
	else if (iblock < SNAPSHOT_BLOCK_OFFSET) {
		/* snapshot reserved blocks */
		if (cmd != SNAPSHOT_READ) {
			snapshot_debug(1, "snapshot (%u) reserved block (%d)"
					" - %s access denied!\n", 
					inode->i_generation, (int)iblock,
					snapshot_cmd_str(cmd));
			return -EPERM;
		}
	}
	else if (ei->i_flags & NEXT3_SNAPFILE_TAKE_FL) {
		/* only allocating blocks is allowed during snapshot take */
		if (cmd != SNAPSHOT_READ && cmd != SNAPSHOT_WRITE) {
			snapshot_debug(1, "snapshot (%u) during 'take'"
					" - %s access denied!\n", 
					inode->i_generation, snapshot_cmd_str(cmd));
			return -EPERM;
		}
	}
	else if (cmd == SNAPSHOT_WRITE) {
		/* snapshot image write access */
		snapshot_debug(1, "snapshot (%u) is read-only"
				" - write access denied!\n", 
				inode->i_generation);
		return -EPERM;
	} 
	else if (cmd == SNAPSHOT_READ) {
		/* snapshot image read access */
		/* no handle or h_level == 0 indicates this is a direct user read */
		if (!handle || handle->h_level == 0)
			/* read access as snapshot file */
			return 1;
	}
#ifdef CONFIG_NEXT3_FS_SNAPSHOT_FILE_SHRINK
	else if (cmd == SNAPSHOT_SHRINK) {
		/* shrink deleted snapshot files */
		return 1;
	}
#endif
	/* access as regular file */
	return 0;
}

/* 
 * next3_snapshot_map_blocks() - helper function for next3_snapshot_test_and_cow() 
 * also used by next3_snapshot_create() to pre-allocate snapshot blocks.
 * check for mapped block (cmd == SNAPSHOT_READ)
 * or map new blocks to snapshot file
 */
int next3_snapshot_map_blocks(handle_t *handle, struct inode *inode,
		next3_snapblk_t block, unsigned long maxblocks, next3_fsblk_t *mapped, int cmd)
{
	struct buffer_head dummy;
	int err;

	dummy.b_state = 0;
	dummy.b_blocknr = -1000;
	err = next3_get_blocks_handle(handle, inode, SNAPSHOT_IBLOCK(block), maxblocks,
					&dummy, cmd);
	/*
	 * next3_get_blocks_handle() returns number of blocks
	 * mapped. 0 in case of a HOLE.
	 */
	if (err <= 0)
		dummy.b_blocknr = 0;
	else if (mapped)
		*mapped = dummy.b_blocknr;

	snapshot_debug_hl(5, "snapshot (%u) map_blocks [%lld/%lld] = [%lld/%lld] "
			"cmd=%d, maxblocks=%lu, mapped=%d\n", 
			inode->i_generation, SNAPSHOT_BLOCK_GROUP_OFFSET(block), SNAPSHOT_BLOCK_GROUP(block),
			SNAPSHOT_BLOCK_GROUP_OFFSET(dummy.b_blocknr), SNAPSHOT_BLOCK_GROUP(dummy.b_blocknr),
			cmd, maxblocks, err);
	return err;
}

/*
 * COW bitmap functions
 */

/*
 * next3_snapshot_read_cow_bitmap() reads the COW bitmap for a given block_group
 * creates the COW bitmap on first time after snapshot take
 * COW bitmap cache is non-persistent, so no need to mark the group desc block dirty
 *
 * Return buffer_head on success or NULL in case of failure.
 */
static struct buffer_head *
next3_snapshot_read_cow_bitmap(handle_t *handle, struct inode *snapshot, 
								unsigned int block_group, int *cowed)
{
	SNAPSHOT_DEBUG_ONCE;
	struct super_block *sb = snapshot->i_sb;
	struct next3_sb_info *sbi = NEXT3_SB(sb);
	struct next3_group_desc * desc;
	struct buffer_head *cow_bh, *bitmap_bh;
#ifdef CONFIG_NEXT3_FS_SNAPSHOT_EXCLUDE_BITMAP
	struct buffer_head *exclude_bitmap_bh = NULL;
#endif
	struct journal_head *jh;
	next3_fsblk_t bitmap_blk;
	next3_fsblk_t cow_bitmap_blk;
	int err;

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
			snapshot_debug(3, "COWing bitmap #%u of snapshot (%u)...\n",
					block_group, snapshot->i_generation);
			/* sleep 1 tunable delay unit */
			snapshot_test_delay(SNAPTEST_BITMAP);
			break;
		} 
		if (cow_bitmap_blk == bitmap_blk) {
			/* wait for another task to COW bitmap block */
			snapshot_debug_once(2, "waiting for pending cow bitmap #%d...\n",
					block_group);
			/* can happen once per block_group/snapshot - just keep trying */
			yield();
		}
	}
#endif

	if (cow_bitmap_blk)
		return sb_bread(sb, cow_bitmap_blk);

#ifdef CONFIG_NEXT3_FS_SNAPSHOT_JOURNAL_CREDITS
	/* 
	 * this is the first time this block group is accessed 
	 * in the current handle, so we have to account for one COW credit
	 * (even if we don't end up COWing this bitmap) -goldor
	 */
	if (cowed)
		*cowed = 1;
#endif

	/* 
	 * read cow bitmap block from snapshot file 
	 * if cow bitmap is not yet allocated,
	 * create the new cow bitmap block
	 */
	cow_bh = next3_bread(handle, snapshot, SNAPSHOT_IBLOCK(bitmap_blk), SNAPSHOT_READ, &err);
	if (cow_bh)
		/* update cow bitmap cache with snapshot existing cow bitmap block */
		goto update_cache;

	/* 
	 * first time cow bitmap access - 
	 * either this is get_undo_access() for block bitmap or 
	 * get_undo_access() for block bitmap has not been requested yet.
	 * either way, besides snapshot file updates, we know that
	 * the bitmap buffer remains unchanged,
	 * so we copy the committed_data to exclude these changes.
	 */
	bitmap_bh = sb_bread(sb, bitmap_blk);
	if (bitmap_bh) {
		err = next3_snapshot_map_blocks(handle, snapshot, bitmap_blk, 1, 
				&cow_bitmap_blk, SNAPSHOT_BITMAP);
		if (err > 0)
			cow_bh = sb_getblk(sb, cow_bitmap_blk);
	}
	if (cow_bh) {
		const char *data = bitmap_bh->b_data;
		const char *mask = NULL;
		
#ifdef CONFIG_NEXT3_FS_SNAPSHOT_EXCLUDE_BITMAP
		exclude_bitmap_bh = read_exclude_bitmap(sb, block_group);
		if (exclude_bitmap_bh)
			/* mask block bitmap with exclude bitmap to create COW bitmap */
			mask = exclude_bitmap_bh->b_data;
#endif	
		/* 
		 * another COWing task may be changing this block bitmap
		 * (allocating active snapshot blocks) while we are trying to copy it.
		 * copying committed_data will keep us protected from these changes.
		 * at this point we are guarantied, that the only difference 
		 * between block bitmap and commited_data are the new active snapshot blocks, 
		 * because before allocating/freeing any other blocks
		 * a task must first get_undo_access() and get here. -goldor
		 */
		jbd_lock_bh_journal_head(bitmap_bh);
		jbd_lock_bh_state(bitmap_bh);
		jh = bh2jh(bitmap_bh);
		if (jh && jh->b_committed_data)
			data = jh->b_committed_data;

		next3_snapshot_copy_to_buffer_sync(cow_bh, bitmap_bh, data, mask);

		jbd_unlock_bh_state(bitmap_bh);
		jbd_unlock_bh_journal_head(bitmap_bh);

		snapshot_debug(3, "COW bitmap #%u of snapshot (%u) "
				"mapped to block [%lu/%lu]\n", 
				block_group, snapshot->i_generation,
				SNAPSHOT_BLOCK_GROUP_OFFSET(cow_bitmap_blk), 
				SNAPSHOT_BLOCK_GROUP(cow_bitmap_blk));
#ifdef CONFIG_NEXT3_FS_SNAPSHOT_JOURNAL_TRACE
		handle->h_cow_bitmaps++;
#endif
	}
	else {
		snapshot_debug(1, "failed to create COW "
				"bitmap %u of snapshot (%u)\n", 
				block_group, snapshot->i_generation);
	}
	brelse(bitmap_bh);
#ifdef CONFIG_NEXT3_FS_SNAPSHOT_EXCLUDE_BITMAP
	brelse(exclude_bitmap_bh);
#endif

update_cache:
	spin_lock(sb_bgl_lock(sbi, block_group));
	if (cow_bh) {
		/* update cow bitmap cache with snapshot cow bitmap block */
		desc->bg_cow_bitmap = cow_bh->b_blocknr;
	} else
		/* reset cow bitmap cache */
		desc->bg_cow_bitmap = 0;
	spin_unlock(sb_bgl_lock(sbi, block_group));

	return cow_bh;
}

/*
 * next3_snapshot_test_cow_bitmap() tests if the block is in use by snapshot
 */
static int
next3_snapshot_test_cow_bitmap(handle_t *handle, struct inode *snapshot, 
		next3_fsblk_t block, struct buffer_head **pcow_bh, int *cowed)
{
	struct buffer_head *cow_bh;
	unsigned long block_group;
	next3_grpblk_t bit;
	struct super_block *sb = snapshot->i_sb;
	struct next3_sb_info *sbi = NEXT3_SB(sb);
	struct next3_super_block *es = sbi->s_es;
	int snapshot_blocks = SNAPSHOT_BLOCKS(snapshot);

	block_group = (block - le32_to_cpu(es->s_first_data_block)) /
		      	NEXT3_BLOCKS_PER_GROUP(sb);
	bit = (block - le32_to_cpu(es->s_first_data_block)) %
		NEXT3_BLOCKS_PER_GROUP(sb);

	/* check if block is in snapshot range (maybe fs was resized) */
	if (block >= snapshot_blocks) {
		snapshot_debug_hl(4, "snapshot (%u) test COW bitmap: "
				"block [%d/%lu] out of range (snapshot_blocks=%d)\n", 
				snapshot->i_generation, bit, block_group, snapshot_blocks);
		return 0;
	}

	cow_bh = next3_snapshot_read_cow_bitmap(handle, snapshot, block_group, cowed);
	if (!cow_bh)
		return -1;
	/* 
	 * if the bit is set in the COW bitmap, 
	 * then this block is in use by some snapshot. 
	 */
	*pcow_bh = cow_bh;
	return next3_test_bit(bit, cow_bh->b_data) ? 1 : 0;
}

#ifdef CONFIG_NEXT3_FS_SNAPSHOT_FILE_EXCLUDE
/*
 * next3_snapshot_clear_cow_bitmap() clears bits from the COW bitmap
 */
static int
next3_snapshot_clear_cow_bitmap(handle_t *handle, struct inode *snapshot, 
		next3_fsblk_t block, int count, struct buffer_head *cow_bh)
{
	unsigned long block_group;
	next3_grpblk_t bit, blocks_per_group;
	struct super_block *sb = snapshot->i_sb;
	struct next3_sb_info *sbi = NEXT3_SB(sb);
	struct next3_super_block *es = sbi->s_es;
	int err = 0, n = 0, cleared = 0;

	if (!cow_bh)
		return 0;

	blocks_per_group = NEXT3_BLOCKS_PER_GROUP(sb);
	block_group = (block - le32_to_cpu(es->s_first_data_block)) /
		      	blocks_per_group;
	bit = (block - le32_to_cpu(es->s_first_data_block)) %
		blocks_per_group;

	/*
	 * COW bitmap snapshot buffers are journalled as metadata,
	 * unlike all other snapshot buffers that are journalled as ordered data.
	 * we get write access to the COW bitmap buffer anyway
	 * just to keep it from being released, 
	 * but we only mark the buffer dirty if we actually changed it. -goldor
	 */
	//err = next3_journal_dirty_data(handle, cow_bh);
	err = next3_journal_get_write_access(handle, cow_bh);
	if (err)
		return err;

	while (count > 0 && bit < blocks_per_group) {
		if (next3_clear_bit_atomic(sb_bgl_lock(sbi, block_group), 
					bit, cow_bh->b_data)) {
			n++;
		}
		else if (n) {
			snapshot_debug(2, "cleared cow_bitmap: [%d-%d/%ld]\n",
					bit-n, bit-1, block_group);
			cleared += n;
			n = 0;
		}
		bit++;
		count--;
	}

	if (n) {
		snapshot_debug(2, "cleared cow_bitmap: [%d-%d/%ld]\n",
				bit-n, bit-1, block_group);
		cleared += n;
	}

	if (cleared)
		//mark_buffer_dirty(cow_bh);
		err = next3_journal_dirty_metadata(handle, cow_bh);

	return err ? err : cleared;
}
#endif

/*
 * COW functions
 */

#ifdef CONFIG_NEXT3_FS_SNAPSHOT_DEBUG
static void
__next3_snapshot_trace_cow(handle_t *handle, struct inode *inode, 
		struct buffer_head *bh, next3_fsblk_t block, int code, const char *fn)
{
	struct super_block *sb = handle->h_transaction->t_journal->j_private;
	struct next3_sb_info *sbi = NEXT3_SB(sb);
	struct next3_super_block * es = sbi->s_es;
	unsigned long block_group = (block - le32_to_cpu(es->s_first_data_block)) /
		      					NEXT3_BLOCKS_PER_GROUP(sb);
	next3_grpblk_t bit = (block - le32_to_cpu(es->s_first_data_block)) %
							NEXT3_BLOCKS_PER_GROUP(sb);
	unsigned long ino = inode ? inode->i_ino : 1;
	unsigned long inode_group = (ino - 1) / NEXT3_INODES_PER_GROUP(sb);
	next3_grpblk_t inode_offset = (ino - 1) % NEXT3_INODES_PER_GROUP(sb);

	snapshot_debug_hl(4, "get_%s_access(i:%d/%ld, b:%d/%ld)"
			" h_ref=%d, h_level=%d, code=%d\n",
			fn, inode_offset, inode_group, bit, block_group, 
			handle->h_ref, handle->h_level, code);
}

#define next3_snapshot_trace_cow(handle, inode, bh, block, code, fn) \
	if (snapshot_enable_debug >= 4)	\
		__next3_snapshot_trace_cow(handle, inode, bh, block, code, fn)
#else
#define next3_snapshot_trace_cow(handle, inode, bh, block, code, fn)
#endif

/*
 * next3_snapshot_test_and_cow() tests if the block should be cowed
 * if the 'cmd' is SNAPSHOT_COPY, try to copy the block to the snapshot
 * if the 'cmd' is SNAPSHOT_MOVE, try to move the block to the snapshot
 * if the 'cmd' is SNAPSHOT_DEL, try to clear the block from COW bitmap
 *
 * if SNAPSHOT_OK or SNAPSHOT_MOVED are returned, 
 * then the block may be safely written over.
 * if SNAPSHOT_COW is returned,
 * next3_snapshot_cow() must be called before writing to the buffer
 */
static int 
next3_snapshot_test_and_cow(handle_t *handle, struct inode *inode, 
		struct buffer_head *bh, next3_fsblk_t block, int *pcount, 
		int cmd, const char *fn)
{
	struct super_block *sb = handle->h_transaction->t_journal->j_private;
	struct inode *snapshot = next3_snapshot_get_active(sb);
	struct buffer_head *sbh = NULL;
	struct buffer_head *cow_bh = NULL;
	struct journal_head *jh;
	next3_fsblk_t blk = 0;
	int err = -EIO;
	int count = pcount ? *pcount : 1;
	int clear = 0;
	int cowed = 0;

	next3_snapshot_trace_cow(handle, inode, bh, block, cmd, fn);
	snapshot_debug_hl(4, "{\n");

	if (handle->h_level++ >= SNAPSHOT_MAX_RECURSION_LEVEL) {
		snapshot_debug_hl(1, "COW error: max recursion level exceeded!\n"); 
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
		(inode == snapshot && cmd != SNAPSHOT_DEL)) {
		snapshot_debug_hl(4, "active snapshot file update - skip block cow!\n"); 
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
			 * this is not the first time this block is being COWed
			 * in the running transaction, so we don't need to COW
			 * and we don't need to account for any COW credits -goldor
			 */
#ifdef CONFIG_NEXT3_FS_SNAPSHOT_JOURNAL_TRACE
			handle->h_cow_ok_jh++;
#endif
			goto skip_cow;
		}
	}
#endif

#ifdef CONFIG_NEXT3_FS_SNAPSHOT_FILE_EXCLUDE
	err = next3_snapshot_excluded(inode);
	if (cmd == SNAPSHOT_DEL) {
		/* excluded file block clear access -
		 * clear bit from COW bitmap */
		clear = SNAPSHOT_DEL;
		cmd = SNAPSHOT_READ;
	}
	else if (err) {
#ifdef CONFIG_NEXT3_FS_SNAPSHOT_EXCLUDE_FILES
		if (err > 0 && cmd == SNAPSHOT_COPY) {
			/* excluded file block write access - 
			 * COW and zero out snapshot block */
			snapshot_debug_hl(4, "file (%lu) excluded from snapshot - "
					"zero out block (%lu) in snapshot\n",
					inode->i_ino, block);
			clear = SNAPSHOT_COPY;
		}
		else
#endif
		if (cmd != SNAPSHOT_READ) {
			/* excluded file block move/delete access
			 * or ignored file block write access - 
			 * verify bit is set in exclude bitmap */
			if (inode)
				snapshot_debug_hl(4, "file (%lu) excluded from snapshot - "
						"clear block (%lu) from COW bitmap\n",
						inode->i_ino, block);
			clear = SNAPSHOT_MOVE;
			cmd = SNAPSHOT_READ;
		}
	}
#endif

#ifdef CONFIG_NEXT3_FS_SNAPSHOT_JOURNAL_CREDITS
	if (!NEXT3_SNAPSHOT_HAS_TRANS_BLOCKS(handle, 1)) {
		snapshot_debug_hl(1, "COW warning: insuffiecient buffer/user credits (%d/%d) "
				"for COW operation?\n", 
				handle->h_buffer_credits, handle->h_user_credits);
	}
#endif

	/* first get the COW bitmap and test if the block needs to be COWed */
	err = next3_snapshot_test_cow_bitmap(handle, snapshot, block, &cow_bh, &cowed);
#ifdef CONFIG_NEXT3_FS_SNAPSHOT_JOURNAL_TRACE
	if (err == 0)
		handle->h_cow_ok_clear++;
#endif
	if (clear < 0)
		/* ignore COW bitmap test result for ignored file blocks */
		err = 0;
	if (err <= 0)
		goto out;

	/* then test if snapshot already has a private copy of the block */
	err = next3_snapshot_map_blocks(handle, snapshot, block, 1, &blk, SNAPSHOT_READ);
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
			/* freeing blocks of newly added block group - don't move them to snapshot */
#ifdef CONFIG_NEXT3_FS_SNAPSHOT_JOURNAL_TRACE
			handle->h_cow_ok_clear++;
#endif
			err = 0;
			goto out;
		}
		/* move block from inode to snapshot */ 
		err = next3_snapshot_map_blocks(handle, snapshot, block, count, NULL, cmd);
		if (err) {
			if (err > 0) {
				count = err;
				if (pcount)
					*pcount = count;
				vfs_dq_free_block(inode, err);
				err = SNAPSHOT_MOVED;
#ifdef CONFIG_NEXT3_FS_SNAPSHOT_FILE_EXCLUDE
				/* set moved blocks in exclude bitmap */
				clear = SNAPSHOT_MOVE;
#endif
#ifdef CONFIG_NEXT3_FS_SNAPSHOT_JOURNAL_CREDITS
				/* 
				 * when moving 'count' blocks to the same indirect block
				 * we have to account for only one COW credit -goldor
				 */
				cowed = 1;
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
	sbh = next3_getblk(handle, snapshot, SNAPSHOT_IBLOCK(block), SNAPSHOT_COPY, &err);
	if (!sbh || err < 0)
		goto out;

	if (err) {
		/* locked buffer - copy block data to snapshot and unlock buffer */
#ifdef CONFIG_NEXT3_FS_SNAPSHOT_RACE_COW
		snapshot_debug(3, "COWing block [%lld/%lld] of snapshot (%u)...\n",
				SNAPSHOT_BLOCK_GROUP_OFFSET(bh->b_blocknr), 
				SNAPSHOT_BLOCK_GROUP(bh->b_blocknr),
				snapshot->i_generation);
		/* sleep 1 tunable delay unit */
		snapshot_test_delay(SNAPTEST_COW);
#endif
		err = next3_snapshot_copy_buffer_ordered(handle, sbh, bh);
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
#ifdef CONFIG_NEXT3_FS_SNAPSHOT_JOURNAL_CREDITS
	/* 
	 * this is the first time this block was COWed
	 * in the current handle, so we have to account for one COW credit
	 * (even if we didn't actually COW this block) -goldor
	 */
	cowed = 1;
#endif

test_pending_cow:
#ifdef CONFIG_NEXT3_FS_SNAPSHOT_RACE_COW
	if (sbh) {
		SNAPSHOT_DEBUG_ONCE;
		while (buffer_new(sbh)) {
			/* wait for pending COW to complete */
			snapshot_debug_once(2, "waiting for pending cow: block = [%lld/%lld]...\n",
					SNAPSHOT_BLOCK_GROUP_OFFSET(bh->b_blocknr), 
					SNAPSHOT_BLOCK_GROUP(bh->b_blocknr));
			/* can happen once per block/snapshot - wait for COW and keep trying */
			wait_on_buffer(sbh);
			yield();
		}
	}
#endif
#ifdef CONFIG_NEXT3_FS_SNAPSHOT_EXCLUDE_FILES
	if (sbh && !blk)
		blk = sbh->b_blocknr;
	if (clear && blk) {
		/* zero out snapshot block data */
		err = next3_snapshot_zero_data_buffer(handle, snapshot, bh->b_blocknr, blk);
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
			 * in the runnning transaction, so we account for one COW credit
			 * (even if we didn't actually COW this block) 
			 * and we update the COW tid in the journal head. -goldor
			 */
			cowed = 1;
			if (!err)
				jh->b_cow_tid = handle->h_transaction->t_tid;
		}
		jbd_unlock_bh_state(bh);
	}
#endif

#ifdef CONFIG_NEXT3_FS_SNAPSHOT_FILE_EXCLUDE
	if (cow_bh && clear == SNAPSHOT_DEL) {
		/* clear snapshot blocks from COW bitmap */
		count = next3_snapshot_clear_cow_bitmap(handle, snapshot, block, count, cow_bh);
		if (count < 0)
			err = count;
		else if (count > 0) {
			cowed = 1; /* clear COW bitmap used buffer credits */
#ifdef CONFIG_NEXT3_FS_SNAPSHOT_JOURNAL_TRACE
			handle->h_cow_cleared++;
#endif
		}
	}
#endif

#ifdef CONFIG_NEXT3_FS_SNAPSHOT_JOURNAL_CREDITS
	if (cowed) {
		/*
		 * we should get here at most once per transaction for each journalled block
		 * and at most once per handle for each moved/copied block
		 * and at most once per handle for each access block group (COW bitmap)
		 */
		//handle->h_cow_credits--;
	}
#endif

	brelse(cow_bh);
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
#define next3_snapshot_test_cow(handle, bh, fn) \
	next3_snapshot_test_and_cow(handle, NULL, bh, bh->b_blocknr, NULL, SNAPSHOT_READ, fn)
/* 
 * tests if the metadata block should be cowed
 * and in case it does, tries to copy the block to the snapshot
 */
#define next3_snapshot_cow(handle, inode, bh, fn) \
	next3_snapshot_test_and_cow(handle, inode, bh, bh->b_blocknr, NULL, SNAPSHOT_COPY, fn)

#ifdef CONFIG_NEXT3_FS_SNAPSHOT_FILE_MOVE
/*
 * tests if the data block should be cowed
 * and in case it does, tries to move the block to the snapshot
 */
#define next3_snapshot_mow(handle, inode, block, num, fn) \
	next3_snapshot_test_and_cow(handle, inode, NULL, block, num, SNAPSHOT_MOVE, fn)
#endif
/*
 * tests if the block should be cowed
 * and in case it does, tries to clear the block from the COW bitmap
 */
#define next3_snapshot_clear(handle, inode, block, num, fn) \
	next3_snapshot_test_and_cow(handle, inode, NULL, block, num, SNAPSHOT_DEL, fn)

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
	if (ret == SNAPSHOT_COW) {
		/* 
		 * we shouldn't get here if everything works properly 
		 * because undo access is only requested for block bitmaps 
		 * which should be COW'ed in next3_snapshot_test_cow_bitmap() -goldor
		 */
		snapshot_debug(1, "warning: block bitmap [%lld/%lld]"
				" needs to be COWed?!\n",
				 SNAPSHOT_BLOCK_GROUP_OFFSET(bh->b_blocknr), 
				 SNAPSHOT_BLOCK_GROUP(bh->b_blocknr));
	}	
	return ret;
}

/*
 * get_create_access() is called after allocating a new metadata block
 */
int next3_snapshot_get_create_access(handle_t *handle, struct buffer_head *bh)
{
	int ret;
	ret = next3_snapshot_test_cow(handle, bh, "create");
	if (ret == SNAPSHOT_COW) {
#ifdef CONFIG_NEXT3_FS_SNAPSHOT_FILE_MOVE
		/* 
		 * we shouldn't get here if get_delete_access() 
		 * was called for all deleted blocks. 
		 * however, we could definetly get here if fsck was run 
		 * and if it had freed some blocks, naturally, 
		 * without moving them to snapshot. -goldor
		 */
		snapshot_debug(1, "warning: new allocated block (%lld)"
				" needs to be COWed?!\n",
				bh->b_blocknr);
#else
		if (handle->h_level == 0) {
			/*
			 * never mind why we got here, we have to try to COW
			 * the new allocated block before it is overwritten.
			 * if allocation is for active snapshot itself (h_level > 0),
			 * there is nothing gracefull we can do, 
			 * so issue an fs error. -goldor
			 */
			/* 
			 * buffer comes here locked and should return locked 
			 * but we must unlock it, because it may not be up-to-date,
			 * so it may be read from disk before it is being COWed.
			 */
			unlock_buffer(bh);
			ret = next3_snapshot_cow(handle, NULL, bh, "create");
			lock_buffer(bh);
		}
#endif
	}
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

