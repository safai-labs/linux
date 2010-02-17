/*
 * linux/fs/next3/snapshot_ctl.c
 *
 * Written by Amir Goldstein <amir@ctera.com>, 2008
 *
 * Copyright (C) 2008-2010 CTERA Networks
 *
 * This file is part of the Linux kernel and is made available under
 * the terms of the GNU General Public License, version 2, or at your
 * option, any later version, incorporated herein by reference.
 *
 * Next3 snapshots control functions.
 */

#include <linux/statfs.h>
#include "snapshot.h"

/*
 * next3_snapshot_set_active() sets the current active snapshot to @inode and
 * returns the deactivated snapshot inode.  If inode is NULL, current active
 * snapshot is deactivated.  This function should be called under
 * journal_lock_updates() and snapshot mutex lock.
 */
static struct inode *
next3_snapshot_set_active(struct super_block *sb, struct inode *inode)
{
	struct inode *old = NEXT3_SB(sb)->s_active_snapshot;

	if (inode && NEXT3_BLOCK_SIZE(sb) != SNAPSHOT_BLOCK_SIZE) {
		snapshot_debug(1, "failed to activate snapshot (%u)"
			       "because file system block size (%lu) != "
			       "page size (%lu)\n", inode->i_generation,
			       NEXT3_BLOCK_SIZE(sb), SNAPSHOT_BLOCK_SIZE);
		return NULL;
	}

	if (old == inode)
		return NULL; /* no snapshot was deactivated */

	if (old) {
		snapshot_debug(1, "snapshot (%u) deactivated\n",
			       old->i_generation);
		NEXT3_I(old)->i_flags &= ~NEXT3_SNAPFILE_ACTIVE_FL;
		/* remove active snapshot reference */
		iput(old);
	}
	if (inode) {
		/* add active snapshot reference */
		if (!igrab(inode))
			return old;
		NEXT3_I(inode)->i_flags |= NEXT3_SNAPFILE_ACTIVE_FL;
		snapshot_debug(1, "snapshot (%u) activated\n",
			       inode->i_generation);
	}
	NEXT3_SB(sb)->s_active_snapshot = inode;

	return old;
}

#ifdef CONFIG_NEXT3_FS_SNAPSHOT_CTL
/*
 * Snapshots control functions
 */
static int next3_snapshot_enable(struct inode *inode);
static int next3_snapshot_disable(struct inode *inode);
static int next3_snapshot_create(struct inode *inode);
static int next3_snapshot_delete(struct inode *inode);
static int next3_snapshot_exclude(handle_t *handle, struct inode *inode);

/*
 * next3_snapshot_get_flags() check snapshot state
 * Called from next3_ioctl() under i_mutex
 */
void next3_snapshot_get_flags(struct next3_inode_info *ei, struct file *filp)
{
	int open_count = atomic_read(&filp->f_path.dentry->d_count);
	/*
	 * 1 count for ioctl (lsattr)
	 * greater count means the snapshot is open by user (mounted?)
	 */
	if (open_count > 1)
		ei->i_flags |= NEXT3_SNAPFILE_OPEN_FL;
	else
		ei->i_flags &= ~NEXT3_SNAPFILE_OPEN_FL;
}

/*
 * next3_snapshot_set_flags() monitors snapshot state changes
 * Called from next3_ioctl() under i_mutex and snapshot_mutex
 */
int next3_snapshot_set_flags(handle_t *handle, struct inode *inode,
			     unsigned int flags)
{
	unsigned int oldflags = NEXT3_I(inode)->i_flags;
	int err = 0;

	if (!((flags | oldflags) & NEXT3_SNAPFILE_FL))
		/* snapshot flags can only be changed for snapshot files */
		goto non_snapshot;

#ifdef CONFIG_NEXT3_FS_SNAPSHOT_DEBUG
	if ((oldflags & NEXT3_SNAPFILE_FL) &&
		(oldflags & NEXT3_NODUMP_FL) &&
		!(flags & NEXT3_NODUMP_FL)) {
		/* print snapshot inode map on chattr -d */
		next3_snapshot_dump(1, inode);
		/* restore the 'No_Dump' flag */
		flags |= NEXT3_NODUMP_FL;
	}
#endif

#ifdef CONFIG_NEXT3_FS_SNAPSHOT_FILE_EXCLUDE
	if ((flags & NEXT3_SNAPFILE_CLEAN_FL) &&
		!(oldflags & NEXT3_SNAPFILE_CLEAN_FL))
		err = next3_snapshot_exclude(handle, inode);
	if (err)
		goto out;
#endif

	if ((flags ^ oldflags) & NEXT3_SNAPFILE_ENABLED_FL) {
		/* enabled/disabled the snapshot during transaction */
		if (flags & NEXT3_SNAPFILE_ENABLED_FL)
			err = next3_snapshot_enable(inode);
		else
			err = next3_snapshot_disable(inode);
	}
	if (err)
		goto out;

	if ((flags ^ oldflags) & NEXT3_SNAPFILE_FL) {
		/* add/delete to snapshots list during transaction */
		if (flags & NEXT3_SNAPFILE_FL)
			err = next3_snapshot_create(inode);
		else
			err = next3_snapshot_delete(inode);
	}
	if (err)
		goto out;

non_snapshot:
	/* set only non-snapshot flags here */
	flags &= ~NEXT3_FL_SNAPSHOT_MASK;
	flags |= (NEXT3_I(inode)->i_flags & NEXT3_FL_SNAPSHOT_MASK);
	NEXT3_I(inode)->i_flags = flags;

out:
	/*
	 * retake reserve inode write from next3_ioctl() and mark inode
	 * dirty
	 */
	next3_mark_inode_dirty(handle, inode);
	return err;
}

/*
 * If we have fewer than nblocks credits,
 * extend transaction by a minimum of NEXT3_MAX_TRANS_DATA.
 * If that fails, restart the transaction &
 * regain write access for the inode block.
 */
static int __extend_or_restart_transaction(const char *where,
		handle_t *handle, struct inode *inode, int nblocks)
{
	int err;

	if (NEXT3_SNAPSHOT_HAS_TRANS_BLOCKS(handle, nblocks))
		return 0;

	if (nblocks < NEXT3_MAX_TRANS_DATA)
		nblocks = NEXT3_MAX_TRANS_DATA;

	err = __next3_journal_extend(where, handle, nblocks);
	if (err < 0)
		return err;
	if (err) {
		if (inode)
			/* lazy way to do mark_iloc_dirty() */
			next3_mark_inode_dirty(handle, inode);
		err = __next3_journal_restart(where, handle, nblocks);
		if (err)
			return err;
		if (inode)
			/* lazy way to do reserve_inode_write() */
			next3_mark_inode_dirty(handle, inode);
	}

	return 0;
}

#define extend_or_restart_transaction(handle, nblocks)			\
	__extend_or_restart_transaction(__func__, (handle), NULL, (nblocks))
#define extend_or_restart_transaction_inode(handle, inode, nblocks)	\
	__extend_or_restart_transaction(__func__, (handle), (inode), (nblocks))

/*
 * next3_snapshot_reset_bitmap_cache():
 *
 * Resets the COW/exclude bitmap cache for all block groups.
 * Helper function for next3_snapshot_take() and
 * next3_snapshot_init_bitmap_cache().
 * COW/exclude bitmap cache is non-persistent, so no need to mark the group
 * desc blocks dirty.  Called under lock_super() or sb_lock
 */
static void next3_snapshot_reset_bitmap_cache(struct super_block *sb, int init)
{
	struct next3_group_desc *desc;
	int i;

	for (i = 0; i < NEXT3_SB(sb)->s_groups_count; i++) {
		desc = next3_get_group_desc(sb, i, NULL);
		if (!desc)
			continue;
		desc->bg_cow_bitmap = 0;
		if (init)
			desc->bg_exclude_bitmap = 0;
	}
}

#ifdef CONFIG_NEXT3_FS_SNAPSHOT_EXCLUDE_INODE
/*
 * next3_exclude_inode_bread - read indirect block from exclude inode
 * @handle:	JBD handle (NULL is !@create)
 * @inode:	exclude inode
 * @grp:	block group
 * @create:	if true, try to allocate missing indirect block
 *
 * Helper function for next3_snapshot_init_bitmap_cache().
 * Called under sb_lock and before snapshots are loaded, so changes made to
 * exclude inode are not COWed.
 *
 * Returns indirect block buffer or NULL if not allocated.
 */
static struct buffer_head *next3_exclude_inode_bread(handle_t *handle,
		struct inode *inode, int grp, int create)
{
	int dind_offset = grp / SNAPSHOT_ADDR_PER_BLOCK;
	struct buffer_head *ind_bh;
	int err;

	/* exclude bitmap blocks addresses are exposed on the IND branch */
	ind_bh = next3_bread(NULL, inode, NEXT3_IND_BLOCK + dind_offset,
						 0, &err);
	if (ind_bh)
		return ind_bh;

	snapshot_debug(1, "failed to read exclude inode indirect[%d] block\n",
			dind_offset);
	if (!create)
		return NULL;

	err = extend_or_restart_transaction(handle, NEXT3_RESERVE_TRANS_BLOCKS);
	if (err)
		return NULL;
	ind_bh = next3_bread(handle, inode, NEXT3_IND_BLOCK + dind_offset,
			create, &err);
	if (!ind_bh) {
		snapshot_debug(1, "failed to allocate exclude "
				"inode indirect[%d] block\n",
				dind_offset);
		return NULL;
	}
	snapshot_debug(2, "allocated exclude bitmap "
			"indirect[%d] block (%lld)\n",
			dind_offset, ind_bh->b_blocknr);
	return ind_bh;
}

/*
 * next3_exclude_inode_getblk - read address of exclude bitmap block
 * @handle:	JBD handle (NULL is !@create)
 * @inode:	exclude inode
 * @grp:	block group
 * @create:	if true, try to allocate missing blocks
  *
 * Helper function for next3_snapshot_init_bitmap_cache().
 * Called under sb_lock and before snapshots are loaded, so changes made to
 * exclude inode are not COWed.
 *
 * Returns exclude bitmap block address (little endian) or 0 if not allocated.
 */
static __le32 next3_exclude_inode_getblk(handle_t *handle,
		struct inode *inode, int grp, int create)
{
	int ind_offset = grp % SNAPSHOT_ADDR_PER_BLOCK;
	struct buffer_head *bh, *ind_bh = NULL;
	__le32 exclude_bitmap = 0;
	int err = 0;

	/* read exclude inode indirect block */
	ind_bh = next3_exclude_inode_bread(handle, inode, grp, create);
	if (!ind_bh)
		return 0;

	if (grp >= NEXT3_SB(inode->i_sb)->s_groups_count)
		/* past last block group - just allocating indirect blocks */
		goto out;

	exclude_bitmap = ((__le32 *)ind_bh->b_data)[ind_offset];
	if (exclude_bitmap)
		goto out;
	if (!create)
		goto alloc_out;

	/* try to allocate missing exclude bitmap(+ind+dind) block */
	err = extend_or_restart_transaction(handle,
			NEXT3_RESERVE_TRANS_BLOCKS);
	if (err)
		goto alloc_out;

	/* exclude bitmap blocks are mapped on the DIND branch */
	bh = next3_getblk(handle, inode, SNAPSHOT_IBLOCK(grp), create, &err);
	if (!bh)
		goto alloc_out;
	brelse(bh);
	exclude_bitmap = ((__le32 *)ind_bh->b_data)[ind_offset];
alloc_out:
	if (exclude_bitmap)
		snapshot_debug(2, "allocated exclude bitmap #%d block "
				"(%u)\n", grp,
				le32_to_cpu(exclude_bitmap));
	else
		snapshot_debug(1, "failed to allocate exclude "
				"bitmap #%d block (err = %d)\n",
				grp, err);
out:
	brelse(ind_bh);
	return exclude_bitmap;
}

/*
 * next3_snapshot_init_bitmap_cache():
 *
 * Init the COW/exclude bitmap cache for all block groups.
 * COW bitmap cache is set to 0 (lazy init on first access to block group).
 * Read exclude bitmap blocks addresses from exclude inode and store them
 * in block group descriptor.  Try to allocate missing exclude bitmap blocks.
 * Exclude bitmap cache is non-persistent, so no need to mark the group
 * desc blocks dirty.
 *
 * Helper function for snapshot_load().  Called under sb_lock.
 */
static void next3_snapshot_init_bitmap_cache(struct super_block *sb)
{
	struct next3_group_desc *desc;
	struct next3_sb_info *sbi = NEXT3_SB(sb);
	handle_t *handle;
	struct inode *inode;
	__le32 exclude_bitmap = 0;
	int grp, max_groups = sbi->s_groups_count;
	int err = 0, create = 0;
	loff_t i_size;

	/* reset COW/exclude bitmap cache */
	next3_snapshot_reset_bitmap_cache(sb, 1);

	if (!NEXT3_HAS_COMPAT_FEATURE(sb,
				      NEXT3_FEATURE_COMPAT_EXCLUDE_INODE)) {
		snapshot_debug(1, "warning: exclude_inode feature not set - "
			       "snapshot merge might not free all unused "
			       "blocks!\n");
		return;
	}
	inode = next3_iget(sb, NEXT3_EXCLUDE_INO);
	if (IS_ERR(inode)) {
		snapshot_debug(1, "warning: bad exclude inode - "
				"no exclude bitmap!\n");
		return;
	}
	/* start large transaction that will be extended/restarted */
	handle = next3_journal_start(inode, NEXT3_MAX_TRANS_DATA);
	if (IS_ERR(handle))
		/* only read allocated exclude bitmap blocks */
		handle = NULL;
	if (handle) {
		/* allocate missing exclude inode blocks */
		create = 1;
		/* number of groups the filesystem can grow to */
		max_groups = sbi->s_gdb_count +
			le16_to_cpu(sbi->s_es->s_reserved_gdt_blocks);
		max_groups *= NEXT3_DESC_PER_BLOCK(sb);
	}

	/*
	 * Init exclude bitmap blocks for all existing block groups and
	 * allocate indirect blocks for all reserved block groups.
	 */
	for (grp = 0; grp < max_groups; grp++) {
		exclude_bitmap = next3_exclude_inode_getblk(handle, inode, grp,
				create);
		if (!exclude_bitmap)
			continue;

		desc = next3_get_group_desc(sb, grp, NULL);
		if (!desc)
			continue;

		desc->bg_exclude_bitmap = exclude_bitmap;
		snapshot_debug(2, "update exclude bitmap #%d cache "
			       "(block=%u)\n", grp,
			       le32_to_cpu(exclude_bitmap));
	}

	i_size = SNAPSHOT_IBLOCK(max_groups) << SNAPSHOT_BLOCK_SIZE_BITS;
	if (!create || NEXT3_I(inode)->i_disksize >= i_size)
		goto out;

	i_size_write(inode, i_size);
	NEXT3_I(inode)->i_disksize = i_size;
	err = next3_mark_inode_dirty(handle, inode);
out:
	if (handle)
		next3_journal_stop(handle);
	iput(inode);
}

#else
/* with no exclude inode, exclude bitmap is reset to 0 */
#define next3_snapshot_init_bitmap_cache(sb)	\
		next3_snapshot_reset_bitmap_cache(sb, 1)
#endif

/*
 * helper function for snapshot_create().
 * places pre-allocated [d,t]ind blocks in position
 * after they have been allocated as direct blocks.
 */
static inline int next3_snapshot_shift_blocks(struct next3_inode_info *ei,
		int from, int to, int count)
{
	int i, err = -EIO;

	/* the ranges must not overlap */
	BUG_ON(from < 0 || from + count > to);
	BUG_ON(to + count > NEXT3_N_BLOCKS);

	/*
	 * truncate_mutex is held whenever allocating or freeing inode
	 * blocks.
	 */
	mutex_lock(&ei->truncate_mutex);

	/*
	 * verify that 'from' blocks are allocated
	 * and that 'to' blocks are not allocated.
	 */
	for (i = 0; i < count; i++)
		if (!ei->i_data[from+i] ||
				ei->i_data[to+i])
			goto out;

	/*
	 * shift 'count' blocks from position 'from' to 'to'
	 */
	for (i = 0; i < count; i++) {
		ei->i_data[to+i] = ei->i_data[from+i];
		ei->i_data[from+i] = 0;
	}
	err = 0;
out:	
	mutex_unlock(&ei->truncate_mutex);
	return err;
}

/*
 * next3_snapshot_create() initilizes a snapshot file
 * and adds it to the list of snapshots
 * Called under i_mutex and snapshot_mutex
 */
static int next3_snapshot_create(struct inode *inode)
{
	handle_t *handle;
	struct inode *active_snapshot = next3_snapshot_has_active(inode->i_sb);
	struct next3_inode_info *ei = NEXT3_I(inode);
	int i, count, err;
	struct buffer_head *bh = NULL;
	struct next3_group_desc *desc;
	unsigned long ino;
	struct next3_iloc iloc;
	next3_fsblk_t bmap_blk = 0, imap_blk = 0, inode_blk = 0;
	next3_fsblk_t prev_inode_blk = 0;
	loff_t snapshot_blocks = le32_to_cpu(NEXT3_SB(inode->i_sb)->
					     s_es->s_blocks_count);
#ifdef CONFIG_NEXT3_FS_SNAPSHOT_LIST
	struct list_head *l, *list = &NEXT3_SB(inode->i_sb)->s_snapshot_list;

	if (!list_empty(list)) {
		struct inode *last_snapshot =
			&list_first_entry(list, struct next3_inode_info,
					  i_orphan)->vfs_inode;
		if (active_snapshot != last_snapshot) {
			snapshot_debug(1, "failed to add snapshot because last"
				       " snapshot (%u) is not active\n",
				       last_snapshot->i_generation);
			return -EPERM;
		}
	}
#else
	if (active_snapshot) {
		snapshot_debug(1, "failed to add snapshot because active "
			       "snapshot (%u) has to be deleted first\n",
			       active_snapshot->i_generation);
		return -EPERM;
	}
#endif

	/*
	 * Verify that all inode's direct blocks are not allocated.
	 * TODO: take truncate_mutex before this test
	 */
	for (i = 0; i < NEXT3_N_BLOCKS; i++) {
		if (ei->i_data[i])
			break;
	}
	/* Don't need i_size_read because we hold i_mutex */
	if (i != NEXT3_N_BLOCKS ||
		inode->i_size > 0 || ei->i_disksize > 0) {
		snapshot_debug(1, "failed to create snapshot file (ino=%lu) "
				"because it is not empty (i_data[%d]=%u, "
				"i_size=%lld, i_disksize=%lld)\n",
				inode->i_ino, i, ei->i_data[i],
				inode->i_size, ei->i_disksize);
		return -EPERM;
	}

	/*
	 * Take a reference to the small transaction that started in
	 * next3_ioctl() We will extend or restart this transaction as we go
	 * along.  journal_start(n > 1) would not have increase the buffer
	 * credits.
	 */
	handle = next3_journal_start(inode, 1);

	err = extend_or_restart_transaction_inode(handle, inode, 2);
	if (err)
		goto out_handle;

	/*
	 * first we mark the file 'snapshot take' and add it to the head
	 * of the snapshot list (in-memory but not on-disk).
	 * at the end of snapshot_take(), it will become the active snapshot.
	 * finally, if snapshot_create() or snapshot_take() has failed,
	 * snapshot_update() will remove it from the head of the list.
	 */
	ei->i_flags |= (NEXT3_SNAPFILE_FL|NEXT3_SNAPFILE_TAKE_FL);
	ei->i_flags &= ~NEXT3_SNAPFILE_ENABLED_FL;

	/* record the new snapshot ID in the snapshot inode generation field */
	inode->i_generation = le32_to_cpu(NEXT3_SB(inode->i_sb)->
					  s_es->s_last_snapshot_id) + 1;
	if (inode->i_generation == 0)
		/* 0 is not a valid snapshot id */
		inode->i_generation = 1;

	/* record the file system size in the snapshot inode disksize field */
	SNAPSHOT_SET_SIZE(inode, snapshot_blocks << SNAPSHOT_BLOCK_SIZE_BITS);
	SNAPSHOT_SET_DISABLED(inode);

#ifdef CONFIG_NEXT3_FS_SNAPSHOT_LIST
	err = next3_inode_list_add(handle, inode, list,
			   &NEXT3_SB(inode->i_sb)->s_es->s_last_snapshot,
			   "snapshot");
	/* add snapshot list reference */
	if (err || !igrab(inode)) {
		snapshot_debug(1, "failed to add snapshot (%u) to list\n",
			       inode->i_generation);
		goto out_handle;
	}
	l = list->next;
#endif

	err = next3_mark_inode_dirty(handle, inode);
	if (err)
		goto out_handle;

	err = extend_or_restart_transaction_inode(handle, inode,
				  SNAPSHOT_META_BLOCKS *
				  NEXT3_DATA_TRANS_BLOCKS(inode->i_sb));
	if (err)
		goto out_handle;
	/* allocate and zero out snapshot meta blocks */
	for (i = 0; i < SNAPSHOT_META_BLOCKS; i++) {
		brelse(bh);
		bh = next3_getblk(handle, inode, i, SNAPMAP_WRITE, &err);
		if (!bh || err)
			break;
		/* zero out meta block and journal as dirty metadata */
		err = next3_journal_get_write_access(handle, bh);
		if (err)
			break;
		lock_buffer(bh);
		memset(bh->b_data, 0, bh->b_size);
		set_buffer_uptodate(bh);
		unlock_buffer(bh);
		err = next3_journal_dirty_metadata(handle, bh);
		if (err)
			break;
	}
	brelse(bh);
	if (!bh || err) {
		snapshot_debug(1, "failed to initiate meta block (%d) "
				"for snapshot (%u)\n",
				i, inode->i_generation);
		goto out_handle;
	}
	/* place pre-allocated [d,t]ind blocks in position */
	err = next3_snapshot_shift_blocks(ei, 
			SNAPSHOT_META_DIND, NEXT3_DIND_BLOCK, 2);
	if (err) {
		snapshot_debug(1, "failed to move pre-allocated [d,t]ind blocks "
				"for snapshot (%u)\n",
				inode->i_generation);
		goto out_handle;
	}

	/* allocate super block and group descriptors for snapshot */
	count = NEXT3_SB(inode->i_sb)->s_gdb_count + 1;
	err = count;
	for (i = 0; err > 0 && i < count; i += err) {
		err = extend_or_restart_transaction_inode(handle, inode,
				NEXT3_DATA_TRANS_BLOCKS(inode->i_sb));
		if (err)
			goto out_handle;
		err = next3_snapshot_map_blocks(handle, inode, i, count - i,
						NULL, SNAPMAP_WRITE);
	}
	if (err <= 0) {
		snapshot_debug(1, "failed to allocate super block and %d "
			       "group descriptor blocks for snapshot (%u)\n",
			       count - 1, inode->i_generation);
		if (err)
			err = -EIO;
		goto out_handle;
	}

	/* start with journal inode and continue with snapshot list */
	ino = NEXT3_JOURNAL_INO;
alloc_inode_blocks:
	/*
	 * pre-allocate the following blocks in the new snapshot:
	 * - block and inode bitmap blocks of ino's block group
	 * - inode table block that contains ino
	 */
	err = extend_or_restart_transaction_inode(handle, inode,
			3 * NEXT3_DATA_TRANS_BLOCKS(inode->i_sb));
	if (err)
		goto out_handle;

	iloc.block_group = 0;
	inode_blk = next3_get_inode_block(inode->i_sb, ino, &iloc);
	if (!inode_blk || inode_blk == prev_inode_blk)
		goto next_snapshot;

	/* not same inode and bitmap blocks as prev snapshot */
	prev_inode_blk = inode_blk;
	bmap_blk = imap_blk = 0;
	desc = next3_get_group_desc(inode->i_sb, iloc.block_group, NULL);
	if (!desc)
		goto next_snapshot;

	bmap_blk = le32_to_cpu(desc->bg_block_bitmap);
	imap_blk = le32_to_cpu(desc->bg_inode_bitmap);
	if (!bmap_blk || !imap_blk)
		goto next_snapshot;

	count = 1;
	if (imap_blk == bmap_blk + 1)
		count++;
	if (inode_blk == imap_blk + 1)
		count++;
	/* try to allocate all blocks at once */
	err = next3_snapshot_map_blocks(handle, inode,
			bmap_blk, count,
			NULL, SNAPMAP_WRITE);
	count = err;
	/* allocate remaining blocks one by one */
	if (err > 0 && count < 2)
		err = next3_snapshot_map_blocks(handle, inode,
				imap_blk, 1,
				NULL,
				SNAPMAP_WRITE);
	if (err > 0 && count < 3)
		err = next3_snapshot_map_blocks(handle, inode,
				inode_blk, 1,
				NULL,
				SNAPMAP_WRITE);
next_snapshot:
	if (!bmap_blk || !imap_blk || !inode_blk || err < 0) {
		next3_fsblk_t blk0 = iloc.block_group *
			NEXT3_BLOCKS_PER_GROUP(inode->i_sb);
		snapshot_debug(1, "failed to allocate block/inode bitmap "
				"or inode table block of inode (%lu) "
				"(%lu,%lu,%lu/%lu) for snapshot (%u)\n",
				ino, bmap_blk - blk0,
				imap_blk - blk0, inode_blk - blk0,
				iloc.block_group, inode->i_generation);
		if (!err)
			err = -EIO;
		goto out_handle;
	}
#ifdef CONFIG_NEXT3_FS_SNAPSHOT_LIST
	if (l != list) {
		ino = list_entry(l, struct next3_inode_info,
				i_orphan)->vfs_inode.i_ino;
		l = l->next;
		goto alloc_inode_blocks;
	}
#endif

	/* sleep 1 tunable delay unit */
	snapshot_test_delay(SNAPTEST_TAKE);
	snapshot_debug(1, "snapshot (%u) created\n", inode->i_generation);
	err = 0;
out_handle:
	next3_journal_stop(handle);
	if (err)
		ei->i_flags &=
			~(NEXT3_SNAPFILE_FL|NEXT3_SNAPFILE_TAKE_FL);
	return err;
}

/*
 * If we call next3_getblk() with NULL handle we will get read through access
 * to snapshot inode.  We don't want read through access in snapshot_take(),
 * so we call next3_getblk() with this dummy handle and since we are not
 * allocating snapshot block here the handle will not be used anyway.
 */
static handle_t dummy_handle;

/*
 * next3_snapshot_copy_block() - copy block to new snapshot
 * @snapshot:	new snapshot to copy block to
 * @bh:		source buffer to be copied
 * @mask:	if not NULL, mask buffer data before copying to snapshot
 * 		(used to mask block bitmap with exclude bitmap)
 * @name:	print name of copied block
 * @idx:	print index of copied block
 *
 * Called from next3_snapshot_take() under journal_lock_updates()
 * Returns snapshot buffer on success, NULL on error
 */
static struct buffer_head *next3_snapshot_copy_block(struct inode *snapshot,
		struct buffer_head *bh, const char *mask,
		const char *name, unsigned long idx)
{
	struct buffer_head *sbh = NULL;
	int err;

	if (!bh)
		return NULL;

	sbh = next3_getblk(&dummy_handle, snapshot,
			SNAPSHOT_IBLOCK(bh->b_blocknr),
			SNAPMAP_READ, &err);

	if (err || !sbh || sbh->b_blocknr == bh->b_blocknr) {
		snapshot_debug(1, "failed to copy %s (%lu) "
				"block [%lld/%lld] to snapshot (%u)\n",
				name, idx,
				SNAPSHOT_BLOCK_GROUP_OFFSET(bh->b_blocknr),
				SNAPSHOT_BLOCK_GROUP(bh->b_blocknr),
				snapshot->i_generation);
		brelse(sbh);
		return NULL;
	}
	
	next3_snapshot_copy_buffer(sbh, bh, mask);

	snapshot_debug(4, "copied %s (%lu) block [%lld/%lld] "
			"to snapshot (%u)\n",
			name, idx,
			SNAPSHOT_BLOCK_GROUP_OFFSET(bh->b_blocknr),
			SNAPSHOT_BLOCK_GROUP(bh->b_blocknr),
			snapshot->i_generation);
	return sbh;
}

/*
 * List of blocks which are copied to snapshot for every special inode.
 * Keep block bitmap first and inode table block last in the list.
 */
enum copy_inode_block {
	COPY_BLOCK_BITMAP,
	COPY_INODE_BITMAP,
	COPY_INODE_TABLE,
	COPY_INODE_BLOCKS_NUM
};

static char *copy_inode_block_name[COPY_INODE_BLOCKS_NUM] = {
	"block bitmap",
	"inode bitmap",
	"inode table"
};

/*
 * next3_snapshot_take() makes a new snapshot file
 * into the active snapshot
 *
 * this function calls journal_lock_updates()
 * and should not be called during a journal transaction
 * Called from next3_ioctl() under i_mutex and snapshot_mutex
 */
int next3_snapshot_take(struct inode *inode)
{
#ifdef CONFIG_NEXT3_FS_SNAPSHOT_LIST
	struct list_head *list = &NEXT3_SB(inode->i_sb)->s_snapshot_list;
	struct list_head *l = list->next;
#endif
	next3_fsblk_t prev_inode_blk = 0;
	struct inode *curr_inode;
	struct super_block *sb = inode->i_sb;
	struct next3_sb_info *sbi = NEXT3_SB(sb);
	struct buffer_head *sbh = NULL;
	struct buffer_head *bhs[COPY_INODE_BLOCKS_NUM] = { NULL };
#ifdef CONFIG_NEXT3_FS_SNAPSHOT_EXCLUDE_BITMAP
	struct buffer_head *exclude_bitmap_bh = NULL;
#endif
	const char *mask = NULL;
	struct next3_super_block *es = NULL;
	struct next3_iloc iloc;
	struct next3_inode *raw_inode, temp_inode;
	struct next3_group_desc *desc;
	int i, err = -EIO;
#ifdef CONFIG_NEXT3_FS_SNAPSHOT_BALLOC_RESERVE
	next3_fsblk_t snapshot_r_blocks;
	struct kstatfs statfs;
#endif

	if (!sbi->s_sbh)
		goto out_err;
	else if (sbi->s_sbh->b_blocknr != 0) {
		snapshot_debug(1, "warning: unexpected super block at block "
			       "(%lld:%d)!\n", sbi->s_sbh->b_blocknr,
			       (char *)sbi->s_es - (char *)sbi->s_sbh->b_data);
	} else if (sbi->s_es->s_magic != cpu_to_le16(NEXT3_SUPER_MAGIC)) {
		snapshot_debug(1, "warning: super block of snapshot (%u) is "
			       "broken!\n", inode->i_generation);
	} else
		sbh = next3_getblk(&dummy_handle, inode, SNAPSHOT_IBLOCK(0),
				   SNAPMAP_READ, &err);

	if (!sbh || sbh->b_blocknr == 0) {
		snapshot_debug(1, "warning: super block of snapshot (%u) not "
			       "allocated\n", inode->i_generation);
		goto out_err;
	} else {
		snapshot_debug(4, "super block of snapshot (%u) mapped to "
			       "block (%lld)\n", inode->i_generation,
			       sbh->b_blocknr);
		es = (struct next3_super_block *)(sbh->b_data +
						  ((char *)sbi->s_es -
						   sbi->s_sbh->b_data));
	}

	err = -EIO;
#ifdef CONFIG_NEXT3_FS_SNAPSHOT_BALLOC_RESERVE
	/* update fs statistics to calculate snapshot reserved space */
	if (next3_statfs_sb(sb, &statfs)) {
		snapshot_debug(1, "failed to statfs before snapshot (%u) "
			       "take\n", inode->i_generation);
		goto out_err;
	}
	/* calculate disk space for potential snapshot file growth based on:
	 * 1 indirect block per 1K fs blocks (to map moved data blocks)
	 * +1 data block per 1K fs blocks (to copy indirect blocks)
	 * +1 data block per fs meta block (to copy meta blocks)
	 * +1 data block per directory (to copy small directory index blocks)
	 * +1 data block per 64 inodes (to copy large directory index blocks)
	 */
	snapshot_r_blocks = 2 * (statfs.f_blocks >>
				 SNAPSHOT_ADDR_PER_BLOCK_BITS) +
		statfs.f_spare[0] + statfs.f_spare[1] +
		(statfs.f_files - statfs.f_ffree) / 64;

	/* verify enough free space before taking the snapshot */
	if (statfs.f_bfree < snapshot_r_blocks) {
		err = -ENOSPC;
		goto out_err;
	}
#endif

	/*
	 * flush journal to disk and clear the RECOVER flag
	 * before taking the snapshot
	 */
	sb->s_op->freeze_fs(sb);
	lock_super(sb);

	/*
	 * copy super block to snapshot and fix it
	 */
	lock_buffer(sbh);
	memcpy(sbh->b_data, sbi->s_sbh->b_data, sb->s_blocksize);
	/*
	 * Convert from Next3 to Ext2 super block:
	 * Remove the has_journal flag and journal inode number.
	 * Remove the has_snapshot flag and last snapshot inode number.
	 * Set the a_snapshot flag to signal fsck this is a snapshot image.
	 */
	es->s_feature_compat &= ~cpu_to_le32(NEXT3_FEATURE_COMPAT_HAS_JOURNAL);
	es->s_journal_inum = 0;
	es->s_feature_ro_compat &= ~cpu_to_le32(NEXT3_FEATURE_RO_COMPAT_HAS_SNAPSHOT);
	es->s_last_snapshot = 0;
	es->s_feature_ro_compat |= cpu_to_le32(NEXT3_FEATURE_RO_COMPAT_A_SNAPSHOT);
	set_buffer_uptodate(sbh);
	unlock_buffer(sbh);
	mark_buffer_dirty(sbh);
	sync_dirty_buffer(sbh);

	/*
	 * copy group descriptors to snapshot
	 */
	for (i = 0; i < sbi->s_gdb_count; i++) {
		brelse(sbh);
		sbh = next3_snapshot_copy_block(inode,
				sbi->s_group_desc[i], NULL,
				"GDT", i);
		if (!sbh)
			goto out_unlockfs;
	}

	/* start with journal inode and continue with snapshot list */
	curr_inode = sbi->s_journal_inode;
copy_inode_blocks:
	/*
	 * copy the following blocks to the new snapshot:
	 * - block and inode bitmap blocks of curr_inode block group
	 * - inode table block that contains curr_inode
	 */
	iloc.block_group = 0;
	err = next3_get_inode_loc(curr_inode, &iloc);
	desc = next3_get_group_desc(sb, iloc.block_group, NULL);
	if (err || !desc) {
		snapshot_debug(1, "failed to read inode and bitmap blocks "
			       "of inode (%lu)\n", curr_inode->i_ino);
		err = err ? : -EIO;
		goto out_unlockfs;
	}
	if (iloc.bh->b_blocknr == prev_inode_blk)
		goto fix_inode_copy;
	prev_inode_blk = iloc.bh->b_blocknr;
	for (i = 0; i < COPY_INODE_BLOCKS_NUM; i++)
		brelse(bhs[i]);
	bhs[COPY_BLOCK_BITMAP] = sb_bread(sb, le32_to_cpu(desc->bg_block_bitmap));
	bhs[COPY_INODE_BITMAP] = sb_bread(sb, le32_to_cpu(desc->bg_inode_bitmap));
	bhs[COPY_INODE_TABLE] = iloc.bh;
#ifdef CONFIG_NEXT3_FS_SNAPSHOT_EXCLUDE_BITMAP
	brelse(exclude_bitmap_bh);
	exclude_bitmap_bh = read_exclude_bitmap(sb, iloc.block_group);
	if (exclude_bitmap_bh)
		/* mask block bitmap with exclude bitmap */
		mask = exclude_bitmap_bh->b_data;
#endif
	err = -EIO;
	for (i = 0; i < COPY_INODE_BLOCKS_NUM; i++) {
		brelse(sbh);
		sbh = next3_snapshot_copy_block(inode, bhs[i], mask,
				copy_inode_block_name[i], curr_inode->i_ino);
		if (!sbh)
			goto out_unlockfs;
		mask = NULL;
	}
fix_inode_copy:
	/* get snapshot copy of raw inode */
	iloc.bh = sbh;
	raw_inode = next3_raw_inode(&iloc);
	if (curr_inode->i_ino == NEXT3_JOURNAL_INO) {
		/*
		 * If we want the snapshot image to pass fsck with no
		 * errors, we need to clear the copy of journal inode,
		 * but we cannot detach these blocks, so we move them
		 * to the copy of the last snapshot inode.
		 */
		memcpy(&temp_inode, raw_inode, sizeof(temp_inode));
		memset(raw_inode, 0, sizeof(*raw_inode));
	} else {
		/*
		 * Snapshot inode blocks are excluded from COW bitmap,
		 * so they appear to be not allocated in the snapshot's
		 * block bitmap.  If we want the snapshot image to pass
		 * fsck with no errors, we need to detach those blocks
		 * from the copy of the snapshot inode.
		 */
		raw_inode->i_size = temp_inode.i_size;
		raw_inode->i_size_high = temp_inode.i_size_high;
		raw_inode->i_blocks = temp_inode.i_blocks;
		memcpy(raw_inode->i_block, temp_inode.i_block,
				sizeof(raw_inode->i_block));
		memset(&temp_inode, 0, sizeof(temp_inode));
	}
	mark_buffer_dirty(sbh);
	sync_dirty_buffer(sbh);

#ifdef CONFIG_NEXT3_FS_SNAPSHOT_LIST
	if (l != list) {
		curr_inode = &list_entry(l, struct next3_inode_info,
				       i_orphan)->vfs_inode;
		l = l->next;
		goto copy_inode_blocks;
	}
#endif

	if (!NEXT3_HAS_RO_COMPAT_FEATURE(sb,
		NEXT3_FEATURE_RO_COMPAT_HAS_SNAPSHOT)) {
		/* If this is the first snapshot
		 * created, add a flag to the superblock.
		 */
		NEXT3_SET_RO_COMPAT_FEATURE(sb,
			NEXT3_FEATURE_RO_COMPAT_HAS_SNAPSHOT);
	}

	/* set as active snapshot on-disk */
#ifdef CONFIG_NEXT3_FS_SNAPSHOT_BALLOC_RESERVE
	sbi->s_es->s_snapshot_r_blocks_count = cpu_to_le32(snapshot_r_blocks);
#endif
	sbi->s_es->s_last_snapshot_id =
		cpu_to_le32(le32_to_cpu(sbi->s_es->s_last_snapshot_id)+1);
	if (sbi->s_es->s_last_snapshot_id == 0)
		/* 0 is not a valid snapshot id */
		sbi->s_es->s_last_snapshot_id = cpu_to_le32(1);
	sbi->s_es->s_last_snapshot = inode->i_ino;
	/*
	 * Set as active snapshot in-memory.
	 * No need to sync the snapshot inode to disk because all
	 * that changes are the snapshot flags TAKE and ACTIVE
	 * which are fixed on load by snapshot_update().
	 * Snapshot data blocks have already been synced to disk.
	 */
	NEXT3_I(inode)->i_flags &= ~NEXT3_SNAPFILE_TAKE_FL;
	next3_snapshot_set_active(sb, inode);
	/* set snapshot file read-only aops */
	next3_set_aops(inode);
	/* reset COW bitmap cache */
	next3_snapshot_reset_bitmap_cache(sb, 0);

	err = 0;
out_unlockfs:
	unlock_super(sb);
	sb->s_op->unfreeze_fs(sb);

	if (err)
		goto out_err;

	/* sleep 1 tunable delay unit */
	snapshot_test_delay(SNAPTEST_TAKE);
	snapshot_debug(1, "snapshot (%u) has been taken\n",
			inode->i_generation);
	next3_snapshot_dump(5, inode);

out_err:
#ifdef CONFIG_NEXT3_FS_SNAPSHOT_EXCLUDE_BITMAP
	brelse(exclude_bitmap_bh);
#endif
	brelse(sbh);
	for (i = 0; i < COPY_INODE_BLOCKS_NUM; i++)
		brelse(bhs[i]);
	return err;
}

#ifdef CONFIG_NEXT3_FS_SNAPSHOT_FILE_EXCLUDE
/*
 * next3_snapshot_clean() "cleans" snapshot file blocks in 1 of 2 ways:
 * 1. from next3_snapshot_remove() with @cleanup=1 to free snapshot file
 *    blocks, before removing snapshot file from snapshots list.
 * 2. from next3_snapshot_exclude() with @cleanup=0 to mark snapshot file
 *    blocks in exclude bitmap.
 * Called under snapshot_mutex.
 *
 * Return values:
 * > 0 - no. of blocks in snapshot file (@cleanup=0)
 * = 0 - successful cleanup (@cleanup=1)
 * < 0 - error
 */
static int next3_snapshot_clean(handle_t *handle, struct inode *inode,
		int cleanup)
{
	struct next3_inode_info *ei = NEXT3_I(inode);
	__le32 *p;
	int i, nblocks = 0;
	int *pblocks = (cleanup ? NULL : &nblocks);

	if (!(ei->i_flags & NEXT3_SNAPFILE_FL)) {
		snapshot_debug(1, "clean of non snapshot file (ino=%lu) "
				"is not allowed.\n",
				inode->i_ino);
		return -EPERM;
	}

	if (ei->i_flags & NEXT3_SNAPFILE_ACTIVE_FL) {
		snapshot_debug(1, "clean of active snapshot (%u) "
			       "is not allowed.\n",
			       inode->i_generation);
		return -EPERM;
	}

	/*
	 * A very simplified version of next3_truncate() for snapshot files.
	 * A non-active snapshot file never allocates new blocks and only frees
	 * blocks under snapshot_mutex, so no need to take truncate_mutex here.
	 * No need to add inode to orphan list for post crash truncate, because
	 * snapshot is still on the snapshot list and marked for deletion.
	 */
	p = ei->i_data;
	for (i = 0; i < NEXT3_N_BLOCKS; i++, p++) {
		int depth = (i < NEXT3_NDIR_BLOCKS ? 0 :
				i - NEXT3_NDIR_BLOCKS + 1);
		if (!*p)
			continue;
		next3_free_branches_cow(handle, inode, NULL,
				p, p+1, depth, pblocks);
		if (cleanup)
			*p = 0;
	}
	return nblocks;
}

/*
 * next3_snapshot_exclude() marks snapshot file blocks in exclude bitmap.
 * Snapshot file blocks should already be excluded if everything works properly.
 * This function is used only to verify the correctness of exclude bitmap.
 * Called under i_mutex and snapshot_mutex.
 */
static int next3_snapshot_exclude(handle_t *handle, struct inode *inode)
{
	int err, nblocks;

	/* extend small transaction started in next3_ioctl() */
	err = extend_or_restart_transaction(handle, NEXT3_MAX_TRANS_DATA);
	if (err)
		return err;

	err = next3_snapshot_clean(handle, inode, 0);
	if (err < 0)
		return err;
	nblocks = err;

	/* mark snapshot 'clean' */
	NEXT3_I(inode)->i_flags |= NEXT3_SNAPFILE_CLEAN_FL;
	err = next3_mark_inode_dirty(handle, inode);
	if (err)
		return err;

	snapshot_debug(1, "snapshot (%u) is clean (%d blocks)\n",
			inode->i_generation, nblocks);
	return 0;
}
#endif

/*
 * next3_snapshot_enable() enables snapshot mount
 * sets the in-use flag and the active snapshot
 * Called under i_mutex and snapshot_mutex
 */
static int next3_snapshot_enable(struct inode *inode)
{
	struct next3_inode_info *ei = NEXT3_I(inode);

	if (!(ei->i_flags & NEXT3_SNAPFILE_FL)) {
		snapshot_debug(1, "next3_snapshot_enable() called with non "
			       "snapshot file (ino=%lu)\n",
			       inode->i_ino);
		return -EINVAL;
	}

	if (ei->i_flags &
	    (NEXT3_SNAPFILE_DELETED_FL|NEXT3_SNAPFILE_TAKE_FL)) {
		snapshot_debug(1, "enable of %s snapshot (%u) "
				"is not permitted\n",
				(ei->i_flags & NEXT3_SNAPFILE_DELETED_FL) ?
				"deleted" : "pre-take",
				inode->i_generation);
		return -EPERM;
	}

	/*
	 * set i_size to block device size to enable loop device mount
	 */
	SNAPSHOT_SET_ENABLED(inode);
	ei->i_flags |= NEXT3_SNAPFILE_ENABLED_FL;

	/* Don't need i_size_read because we hold i_mutex */
	snapshot_debug(4, "setting snapshot (%u) i_size to (%lld)\n",
			inode->i_generation, inode->i_size);
	snapshot_debug(1, "snapshot (%u) enabled\n", inode->i_generation);
	return 0;
}

/*
 * next3_snapshot_disable() disables snapshot mount
 * Called under i_mutex and snapshot_mutex
 */
static int next3_snapshot_disable(struct inode *inode)
{
	struct next3_inode_info *ei = NEXT3_I(inode);

	if (!(ei->i_flags & NEXT3_SNAPFILE_FL)) {
		snapshot_debug(1, "next3_snapshot_disable() called with non "
			       "snapshot file (ino=%lu)\n", inode->i_ino);
		return -EINVAL;
	}

	if (ei->i_flags & NEXT3_SNAPFILE_OPEN_FL) {
		snapshot_debug(1, "disable of mounted snapshot (%u) "
				"is not permitted\n",
				inode->i_generation);
		return -EPERM;
	}

	/*
	 * set i_size to zero to disable loop device mount
	 */
	SNAPSHOT_SET_DISABLED(inode);
	ei->i_flags &= ~NEXT3_SNAPFILE_ENABLED_FL;

	/* invalidate page cache */
	truncate_inode_pages(&inode->i_data, SNAPSHOT_BYTES_OFFSET);

	/* Don't need i_size_read because we hold i_mutex */
	snapshot_debug(4, "setting snapshot (%u) i_size to (%lld)\n",
			inode->i_generation, inode->i_size);
	snapshot_debug(1, "snapshot (%u) disabled\n", inode->i_generation);
	return 0;
}

/*
 * next3_snapshot_delete() marks snapshot for deletion
 * Called under i_mutex and snapshot_mutex
 */
static int next3_snapshot_delete(struct inode *inode)
{
	struct next3_inode_info *ei = NEXT3_I(inode);

	if (ei->i_flags & NEXT3_SNAPFILE_ENABLED_FL) {
		snapshot_debug(1, "delete of enabled snapshot (%u) "
				"is not permitted\n",
				inode->i_generation);
		return -EPERM;
	}

	/* mark deleted for later cleanup to finish the job */
	ei->i_flags |= NEXT3_SNAPFILE_DELETED_FL;
	snapshot_debug(1, "snapshot (%u) marked for deletion\n",
			inode->i_generation);
	return 0;
}
#endif

/*
 * next3_snapshot_remove() removes a snapshot @inode from the list
 * of snapshots stored on disk and truncates the snapshot inode
 * Called from next3_snapshot_merge() under snapshot_mutex
 * Called from next3_snapshot_update() under snapshot_mutex
 */
static int next3_snapshot_remove(struct inode *inode)
{
	handle_t *handle;
	struct next3_sb_info *sbi;
	struct next3_inode_info *ei = NEXT3_I(inode);
	int err = 0;

	/* elevate ref count until final cleanup */
	if (!igrab(inode))
		return 0;

	if (ei->i_flags & (NEXT3_SNAPFILE_ENABLED_FL | NEXT3_SNAPFILE_INUSE_FL
			   | NEXT3_SNAPFILE_ACTIVE_FL)) {
		snapshot_debug(4, "deferred delete of %s snapshot (%u)\n",
				(ei->i_flags & NEXT3_SNAPFILE_ACTIVE_FL) ?
				"active" : 
				((ei->i_flags & NEXT3_SNAPFILE_ENABLED_FL) ?
				"enabled" : "referenced"),
			       inode->i_generation);
		goto out_err;
	}

	/* start large truncate transaction that will be extended/restarted */
	handle = next3_journal_start(inode, NEXT3_MAX_TRANS_DATA);
	if (IS_ERR(handle)) {
		err = PTR_ERR(handle);
		goto out_err;
	}
	sbi = NEXT3_SB(inode->i_sb);

#ifdef CONFIG_NEXT3_FS_SNAPSHOT_FILE_EXCLUDE
	err = next3_snapshot_clean(handle, inode, 1);
	if (err)
		goto out_handle;

	/* reset snapshot inode size */
	i_size_write(inode, 0);
	ei->i_disksize = 0;
	err = next3_mark_inode_dirty(handle, inode);
	if (err)
		goto out_handle;
#endif

	err = extend_or_restart_transaction_inode(handle, inode, 2);
	if (err)
		goto out_err;

	/*
	 * at this point, this snapshot is empty
	 * but still on the snapshots list.
	 * after it is removed from the list,
	 * it must not have the SNAPFILE flag set,
	 * but it may have the SNAPFILE_DELETED flag set.
	 * inode will be marked dirty by next3_inode_list_del()
	 */
	ei->i_flags &= ~NEXT3_SNAPFILE_FL;
	ei->i_flags &= ~NEXT3_FL_SNAPSHOT_DYN_MASK;

#ifdef CONFIG_NEXT3_FS_SNAPSHOT_LIST
	err = next3_inode_list_del(handle, inode,
				   &NEXT3_SB(inode->i_sb)->s_snapshot_list,
				   &sbi->s_es->s_last_snapshot, "snapshot");
	if (err)
		goto out_handle;
	/* remove snapshot list reference */
	iput(inode);
#else
	lock_super(inode->i_sb);
	err = next3_journal_get_write_access(handle, sbi->s_sbh);
	sbi->s_es->s_last_snapshot = 0;
	if (!err)
		err = next3_journal_dirty_metadata(handle, sbi->s_sbh);
	unlock_super(inode->i_sb);
#endif

out_handle:
	next3_journal_stop(handle);
	if (err)
		goto out_err;

	/* sleep 1 tunable delay unit */
	snapshot_test_delay(SNAPTEST_DELETE);
	snapshot_debug(1, "snapshot (%u) deleted\n", inode->i_generation);

	err = 0;
out_err:
	/* drop final ref count */
	iput(inode);
	if (err) {
		snapshot_debug(1, "failed to delete snapshot (%u)\n",
				inode->i_generation);
	}
	return err;
}

#ifdef CONFIG_NEXT3_FS_SNAPSHOT_FILE_SHRINK
/*
 * next3_snapshot_shrink_range - free unused blocks from deleted snapshots
 * @handle: JBD handle for this transaction
 * @start:	latest non-deleted snapshot before deleted snapshots group
 * @end:	first non-deleted snapshot after deleted snapshot group
 * @iblock:	inode offset to first data block to shrink
 * @maxblocks:	inode range of data blocks to shrink
 * @cow_bh:	buffer head to map the COW bitmap block of snapshot @start
 *		if NULL, don't look for COW bitmap block
 *
 * Shrinks @maxblocks blocks starting at inode offset @iblock in a group of
 * subsequent deteted snapshots starting after @start and ending before @end.
 * Shrinking is done by finding a range of mapped blocks in @start snapshot
 * or in one of the deleted snapshots, where no other blocks are mapped in the
 * same range in @start snapshot or in snapshots between them.
 * The blocks in the found range may be 'in-use' by @start snapshot, so only
 * blocks which are not set in the COW bitmap are freed.
 * All mapped block of other deleted snapshots in the same range are freed.
 *
 * Called from next3_snapshot_shrink() under snapshot_mutex.
 * Returns the shrunk blocks range and <0 on error.
 */
static int next3_snapshot_shrink_range(handle_t *handle,
		struct inode *start, struct inode *end,
		sector_t iblock, unsigned long maxblocks,
		struct buffer_head *cow_bh)
{
	struct next3_sb_info *sbi = NEXT3_SB(start->i_sb);
	struct list_head *l;
	struct inode *inode = start;
	/* start with @maxblocks range and narrow it down */
	int err, count = maxblocks;
	/* @start snapshot blocks should not be freed only counted */
	int mapped, shrink = 0;

	/* iterate on (@start <= snapshot < @end) */
	list_for_each_prev(l, &NEXT3_I(start)->i_orphan) {
		err = next3_snapshot_shrink_blocks(handle, inode,
				iblock, count, cow_bh, shrink, &mapped);
		if (err < 0)
			return err;

		/* 0 < new range <= old range */
		BUG_ON(!err || err > count);
		count = err;

		if (!cow_bh)
			/* no COW bitmap - free all blocks in range */
			shrink = -1;
		else if (!shrink)
			/* past @start snapshot - free unused blocks in range */
			shrink = 1;
		else if (mapped)
			/* past first mapped range - free all blocks in range */
			shrink = -1;

		if (l == &sbi->s_snapshot_list)
			/* didn't reach @end */
			return -EINVAL;
		inode = &list_entry(l, struct next3_inode_info,
						  i_orphan)->vfs_inode;
		if (inode == end)
			break;
	}
	return count;
}

/*
 * next3_snapshot_shrink - free unused blocks from deleted snapshot files
 * @handle: JBD handle for this transaction
 * @start:	latest non-deleted snapshot before deleted snapshots group
 * @end:	first non-deleted snapshot after deleted snapshot group
 * @need_shrink: no. of deleted snapshots in the group
 *
 * Frees all blocks in subsequent deteted snapshots starting after @start and
 * ending before @end, except for blocks which are 'in-use' by @start snapshot.
 * (blocks 'in-use' are set in snapshot COW bitmap and not copied to snapshot).
 * Called from next3_snapshot_update() under snapshot_mutex.
 * Returns 0 on success and <0 on error.
 */
static int next3_snapshot_shrink(struct inode *start, struct inode *end,
				 int need_shrink)
{
	struct list_head *l;
	handle_t *handle;
	struct buffer_head cow_bitmap, *cow_bh = NULL;
	next3_fsblk_t block = 0;
	struct next3_sb_info *sbi = NEXT3_SB(start->i_sb);
	int snapshot_blocks = SNAPSHOT_BLOCKS(start);
	unsigned long count = le32_to_cpu(sbi->s_es->s_blocks_count);
	unsigned long block_groups = sbi->s_groups_count;
	long block_group = -1;
	next3_fsblk_t bg_boundary = 0;
	int err;

	snapshot_debug(3, "snapshot (%u-%u) shrink: "
			"count = 0x%lx, need_shrink = %d\n",
			start->i_generation, end->i_generation,
			count, need_shrink);

	/* start large truncate transaction that will be extended/restarted */
	handle = next3_journal_start(start, NEXT3_MAX_TRANS_DATA);
	if (IS_ERR(handle))
		return PTR_ERR(handle);

	while (count > 0) {
		while (block >= bg_boundary) {
			/* sleep 1/block_groups tunable delay unit */
			snapshot_test_delay_per_ticks(SNAPTEST_DELETE,
						      block_groups);
			/* reset COW bitmap cache */
			cow_bitmap.b_state = 0;
			cow_bitmap.b_blocknr = 0;
			cow_bh = &cow_bitmap;
			bg_boundary += SNAPSHOT_BLOCKS_PER_GROUP;
			block_group++;
			if (block >= snapshot_blocks)
				/*
				 * Past last snapshot block group - pass NULL
				 * cow_bh to next3_snapshot_shrink_range().
				 * This will cause snapshots after resize to
				 * shrink to the size of @start snapshot.
				 */
				cow_bh = NULL;
		}

		err = extend_or_restart_transaction(handle,
						    NEXT3_MAX_TRANS_DATA);
		if (err)
			goto out_err;

		err = next3_snapshot_shrink_range(handle, start, end,
					      SNAPSHOT_IBLOCK(block), count,
					      cow_bh);

		snapshot_debug(3, "snapshot (%u-%u) shrink: "
				"block = 0x%lx, count = 0x%lx, err = 0x%x\n",
				start->i_generation, end->i_generation,
				block, count, err);

		if (buffer_mapped(&cow_bitmap) && buffer_new(&cow_bitmap)) {
			snapshot_debug(2, "snapshot (%u-%u) shrink: "
					"block group = %ld/%lu, "
				       "COW bitmap = [%lld/%lld]\n",
				       start->i_generation, end->i_generation,
				       block_group, block_groups,
				       SNAPSHOT_BLOCK_GROUP_OFFSET(
					       cow_bitmap.b_blocknr),
				       SNAPSHOT_BLOCK_GROUP(
					       cow_bitmap.b_blocknr));
			clear_buffer_new(&cow_bitmap);
		}

		if (err <= 0)
			goto out_err;

		block += err;
		count -= err;
	}

	/* marks need_shrink snapshots shrunk */
	err = extend_or_restart_transaction(handle, need_shrink);
	if (err)
		goto out_err;

#ifdef CONFIG_NEXT3_FS_SNAPSHOT_LIST
	/* iterate on (@start < snapshot < @end) */
	list_for_each_prev(l, &NEXT3_I(start)->i_orphan) {
		struct next3_inode_info *ei;
		struct next3_iloc iloc;
		if (l == &sbi->s_snapshot_list)
			break;
		ei = list_entry(l, struct next3_inode_info, i_orphan);
		if (&ei->vfs_inode == end)
			break;
		if (ei->i_flags & NEXT3_SNAPFILE_DELETED_FL &&
			!(ei->i_flags &
			(NEXT3_SNAPFILE_SHRUNK_FL|NEXT3_SNAPFILE_ACTIVE_FL))) {
			/* mark snapshot shrunk */
			err = next3_reserve_inode_write(handle, &ei->vfs_inode,
							&iloc);
			ei->i_flags |= NEXT3_SNAPFILE_SHRUNK_FL;
			if (!err)
				next3_mark_iloc_dirty(handle, &ei->vfs_inode,
						      &iloc);
			if (--need_shrink <= 0)
				break;
		}
	}
#endif

	err = 0;
out_err:
	next3_journal_stop(handle);
	if (need_shrink)
		snapshot_debug(1, "snapshot (%u-%u) shrink: "
			       "need_shrink=%d(>0!), err=%d\n",
			       start->i_generation, end->i_generation,
			       need_shrink, err);
	return err;
}
#endif

#ifdef CONFIG_NEXT3_FS_SNAPSHOT_FILE_MERGE
/*
 * next3_snapshot_merge - merge deleted snapshots
 * @handle: JBD handle for this transaction
 * @start:	latest non-deleted snapshot before deleted snapshots group
 * @end:	first non-deleted snapshot after deleted snapshot group
 * @need_merge: no. of deleted snapshots in the group
 *
 * Move all blocks from deteted snapshots group starting after @start and
 * ending before @end to @start snapshot.  All moved blocks are 'in-use' by
 * @start snapshot, because these deleted snapshot have already been shrunk
 * (blocks 'in-use' are set in snapshot COW bitmap and not copied to snapshot).
 * Called from next3_snapshot_update() under snapshot_mutex.
 * Returns 0 on success and <0 on error.
 */
static int next3_snapshot_merge(struct inode *start, struct inode *end,
				int need_merge)
{
	struct list_head *l, *n;
	handle_t *handle = NULL;
	struct next3_sb_info *sbi = NEXT3_SB(start->i_sb);
	int snapshot_blocks = SNAPSHOT_BLOCKS(start);
	int err;

	snapshot_debug(3, "snapshot (%u-%u) merge: need_merge=%d\n",
			start->i_generation, end->i_generation, need_merge);

	/* iterate safe on (@start < snapshot < @end) */
	list_for_each_prev_safe(l, n, &NEXT3_I(start)->i_orphan) {
		struct next3_inode_info *ei = list_entry(l,
						 struct next3_inode_info,
						 i_orphan);
		struct inode *inode = &ei->vfs_inode;
		next3_fsblk_t block = 0;
		int count = snapshot_blocks;

		if (n == &sbi->s_snapshot_list || inode == end ||
			!(ei->i_flags & NEXT3_SNAPFILE_SHRUNK_FL))
			break;

		/* start large transaction that will be extended/restarted */
		handle = next3_journal_start(inode, NEXT3_MAX_TRANS_DATA);
		if (IS_ERR(handle))
			return PTR_ERR(handle);

		while (count > 0) {
			/* we modify one indirect block and the inode itself
			 * for both the source and destination inodes */
			err = extend_or_restart_transaction(handle, 4);
			if (err)
				goto out_err;

			err = next3_snapshot_merge_blocks(handle, inode, start,
						 SNAPSHOT_IBLOCK(block), count);

			snapshot_debug(3, "snapshot (%u) -> snapshot (%u) "
				       "merge: block = 0x%lx, count = 0x%x, "
				       "err = 0x%x\n", inode->i_generation,
				       start->i_generation, block, count, err);

			if (err <= 0)
				goto out_err;

			block += err;
			count -= err;
		}

		next3_journal_stop(handle);
		handle = NULL;

		/* we finished moving all blocks of interest from 'inode'
		 * into 'start' so it is now safe to remove 'inode' from the
		 * snapshots list forever */
		next3_snapshot_remove(inode);

		if (--need_merge <= 0)
			break;
	}

	err = 0;
out_err:
	if (handle)
		next3_journal_stop(handle);
	if (need_merge)
		snapshot_debug(1, "snapshot (%u-%u) merge: need_merge=%d(>0!), "
			       "err=%d\n", start->i_generation,
			       end->i_generation, need_merge, err);
	return err;
}
#endif

/*
 * next3_snapshot_cleanup - shrink/merge/remove snapshot marked for deletion
 * @inode - inode in question
 * @used_by - latest non-deleted snapshot
 * @deleted - true if snapshot is marked for deletion and not active
 * @need_shrink - counter of deleted snapshots to shrink
 * @need_merge - counter of deleted snapshots to merge
 *
 * Deleted snapshot with no older non-deleted snapshot - remove from list
 * Deleted snapshot with no older enabled snapshot - add to merge count
 * Deleted snapshot with older enabled snapshot - add to shrink count
 * Non-deleted snapshot - shrink and merge deleted snapshots group
 *
 * Called from next3_snapshot_update() under snapshot_mutex
 */
static void next3_snapshot_cleanup(struct inode *inode, struct inode *used_by,
		int deleted, int *need_shrink, int *need_merge)
{
	struct next3_inode_info *ei = NEXT3_I(inode);

	if (deleted && !used_by) {
		/* remove permanently unused deleted snapshot */
		next3_snapshot_remove(inode);
		return;
	}

	if (deleted) {
		/* deleted (non-active) snapshot file */
#ifdef CONFIG_NEXT3_FS_SNAPSHOT_FILE_SHRINK
		if (!(ei->i_flags & NEXT3_SNAPFILE_SHRUNK_FL))
			/* deleted snapshot needs shrinking */
			(*need_shrink)++;
#endif
#ifdef CONFIG_NEXT3_FS_SNAPSHOT_FILE_MERGE
		if (!(ei->i_flags & NEXT3_SNAPFILE_INUSE_FL))
			/* temporarily unused deleted
			 * snapshot needs merging */
			(*need_merge)++;
#endif
	} else {
		/* non-deleted (or active) snapshot file */
#ifdef CONFIG_NEXT3_FS_SNAPSHOT_FILE_SHRINK
		if (*need_shrink)
			/* pass 1: shrink all deleted snapshots
			 * between 'used_by' and 'inode' */
			next3_snapshot_shrink(used_by, inode,
					*need_shrink);
		*need_shrink = 0;
#endif
#ifdef CONFIG_NEXT3_FS_SNAPSHOT_FILE_MERGE
		if (*need_merge)
			/* pass 2: merge all shrunk snapshots
			 * between 'used_by' and 'inode' */
			next3_snapshot_merge(used_by, inode,
					*need_merge);
		*need_merge = 0;
#endif
	}
}


/*
 * Snapshot constructor/destructor
 */

/*
 * next3_snapshot_load - load the on-disk snapshot list to memory
 * Start with last (active) snapshot and continue to older snapshots.
 * If active snapshot load fails, force read-only mount.
 * If at any point in the list load fails, all older snapshot are discarded
 * and remain 'zombies snapshots'.
 * Called from next3_fill_super() under sb_lock
 *
 * Return values:
 * = 0 - on-disk snapshot list is empty or active snapshot loaded
 * < 0 - error loading last (active) snapshot
 */
int next3_snapshot_load(struct super_block *sb, struct next3_super_block *es,
		int read_only)
{
	__le32 *ino_next = &es->s_last_snapshot;
	int num = 0, snapshot_id = 0, has_snapshot = 1;

#ifdef CONFIG_NEXT3_FS_SNAPSHOT_LIST
	if (!list_empty(&NEXT3_SB(sb)->s_snapshot_list)) {
		snapshot_debug(1, "warning: snapshots already loaded!\n");
		return -EINVAL;
	}
#endif

#ifdef CONFIG_NEXT3_FS_SNAPSHOT_JOURNAL_CREDITS
	if (!NEXT3_HAS_COMPAT_FEATURE(sb,
		NEXT3_FEATURE_COMPAT_BIG_JOURNAL))
		snapshot_debug(1, "warning: big_journal feature is not set - "
			       "this might affect concurrnet filesystem "
			       "writers performance!\n");
#endif
	if (*ino_next && !NEXT3_HAS_RO_COMPAT_FEATURE(sb,
				NEXT3_FEATURE_RO_COMPAT_HAS_SNAPSHOT)) {
		/*
		 * When mounting an ext3 formatted volume as next3, the
		 * has_snapshot flag is set on first snapshot_take()
		 * and after that the volume can no longer be mounted
		 * as rw ext3 (only rw next3 or ro ext3/ext2).
		 * Never mind why we got here, but we found a last_snapshot
		 * inode, so will try to load it.  If we succeed, we will
		 * fix the missing has_snapshot flag and if we fail we will
		 * clear the last_snapshot field and allow rw mount.
		 */
		snapshot_debug(1, "warning: has_snapshot feature is not set and"
			       " last snapshot found (%u) - trying to load it\n",
			       le32_to_cpu(*ino_next));
		has_snapshot = 0;
	}

	/* init COW bitmap and exclude bitmap cache */
	next3_snapshot_init_bitmap_cache(sb);

	while (*ino_next) {
		struct inode *inode;

		inode = next3_orphan_get(sb, le32_to_cpu(*ino_next));
		if (IS_ERR(inode) ||
			!(NEXT3_I(inode)->i_flags & NEXT3_FL_SNAPSHOT_MASK)) {
			if (num == 0 && has_snapshot) {
				snapshot_debug(1, "warning: failed to load "
						"active snapshot (ino=%u) - "
						"forcing read-only mount!\n",
						le32_to_cpu(*ino_next));
				return read_only ? 0 : -EINVAL;
			}
			snapshot_debug(1, "warning: failed to load snapshot "
					"(ino=%u) after snapshot (%d) - "
					"terminating snapshot list!\n",
				       le32_to_cpu(*ino_next), snapshot_id);
			*ino_next = 0;
			break;
		}

		snapshot_id = inode->i_generation;
		snapshot_debug(1, "snapshot (%d) loaded\n",
			       snapshot_id);
		num++;
		next3_snapshot_dump(5, inode);

		if (!has_snapshot) {
			NEXT3_SET_RO_COMPAT_FEATURE(sb,
				    NEXT3_FEATURE_RO_COMPAT_HAS_SNAPSHOT);
			snapshot_debug(1, "fixed missing has_snapshot "
				       "flag!\n");
			has_snapshot = 1;
		}

		if (!NEXT3_SB(sb)->s_active_snapshot)
			next3_snapshot_set_active(sb, inode);

#ifdef CONFIG_NEXT3_FS_SNAPSHOT_LIST
		list_add_tail(&NEXT3_I(inode)->i_orphan,
			      &NEXT3_SB(sb)->s_snapshot_list);
		ino_next = &NEXT_ORPHAN(inode);
		/* keep snapshot list reference */
#else
		iput(inode);
		break;
#endif
	}

	snapshot_debug(1, "%d snapshots loaded\n", num);
	if (num > 0)
		next3_snapshot_update(sb, 0);
	return 0;
}

/*
 * next3_snapshot_destroy() releases the in-memoery snapshot list
 * Called from next3_put_super() under big kernel lock
 */
void next3_snapshot_destroy(struct super_block *sb)
{
#ifdef CONFIG_NEXT3_FS_SNAPSHOT_LIST
	struct list_head *l, *n;
	/* iterate safe because we are deleting from list and freeing the
	 * inodes */
	list_for_each_safe(l, n, &NEXT3_SB(sb)->s_snapshot_list) {
		struct inode *inode = &list_entry(l, struct next3_inode_info,
						  i_orphan)->vfs_inode;
		list_del_init(&NEXT3_I(inode)->i_orphan);
		/* remove snapshot list reference */
		iput(inode);
	}
#endif
	/* if there is an active snapshot - deactivate it */
	next3_snapshot_set_active(sb, NULL);
}

/*
 * next3_snapshot_update - iterate snapshot list and update snapshots status
 * If @cleanup is true, shrink/merge/cleanup all snapshots marked for deletion.
 * Called from next3_ioctl() under snapshot_mutex
 * Called from snapshot_load() under sb_lock
 */
void next3_snapshot_update(struct super_block *sb, int cleanup)
{
	struct inode *active_snapshot;
	struct inode *used_by = NULL; /* last non-deleted snapshot found */
	struct list_head *prev;
	struct inode *inode;
	struct next3_inode_info *ei;
	int found_active = 0;
	int found_enabled = 0;
	int deleted;
	int need_shrink = 0;
	int need_merge = 0;

	active_snapshot = next3_snapshot_has_active(sb);
	if (!active_snapshot)
		return;

#ifdef CONFIG_NEXT3_FS_SNAPSHOT_LIST
	/* iterate safe from oldest snapshot backwards */
	prev = NEXT3_SB(sb)->s_snapshot_list.prev;
	if (list_empty(prev))
		return;
#else
	prev = &NEXT3_I(active_snapshot)->i_orphan;
#endif
update_snapshot:
	ei = list_entry(prev, struct next3_inode_info, i_orphan);
	inode = &ei->vfs_inode;
	prev = ei->i_orphan.prev;

	/* all snapshots on the list must have the SNAPSHOT flag and
	 * are not zombies */
	ei->i_flags |= NEXT3_SNAPFILE_FL;
	ei->i_flags &= ~NEXT3_SNAPFILE_ZOMBIE_FL;
	/* set the 'No_Dump' flag on all snapshots */
	ei->i_flags |= NEXT3_NODUMP_FL;

	/* snapshots later than active (failed take) should be removed */
	if (found_active || (ei->i_flags & NEXT3_SNAPFILE_TAKE_FL)) {
		next3_snapshot_remove(inode);
		goto prev_snapshot;
	}

	/*
	 * after completion of a snapshot management operation,
	 * only the active snapshot can have the ACTIVE flag
	 */
	if (inode == active_snapshot) {
		ei->i_flags |= NEXT3_SNAPFILE_ACTIVE_FL;
		found_active = 1;
	} else
		ei->i_flags &= ~NEXT3_SNAPFILE_ACTIVE_FL;

	if (found_enabled)
		/* snapshot is in use by an older enabled snapshot */
		ei->i_flags |= NEXT3_SNAPFILE_INUSE_FL;
	else
		/* snapshot is not in use by older enabled snapshots */
		ei->i_flags &= ~NEXT3_SNAPFILE_INUSE_FL;

	deleted = ((ei->i_flags & NEXT3_SNAPFILE_DELETED_FL) &&
			!(ei->i_flags & NEXT3_SNAPFILE_ACTIVE_FL));
	if (cleanup)
		next3_snapshot_cleanup(inode, used_by, deleted,
				&need_shrink, &need_merge);
	if (!deleted) {
		if (!found_active)
			/* newer snapshot are potentialy used by
			 * this snapshot (when it is enabled) */
			used_by = inode;
		if (ei->i_flags & NEXT3_SNAPFILE_ENABLED_FL)
			found_enabled = 1;
	}

prev_snapshot:
#ifdef CONFIG_NEXT3_FS_SNAPSHOT_LIST
	if (prev != &NEXT3_SB(sb)->s_snapshot_list)
		goto update_snapshot;
#endif

	/* if all snapshots are deleted - deactivate active snapshot */
	deleted = NEXT3_I(active_snapshot)->i_flags & NEXT3_SNAPFILE_DELETED_FL;
	if (cleanup && deleted && !used_by && igrab(active_snapshot)) {
		/* lock journal updates before deactivating snapshot */
		journal_lock_updates(NEXT3_SB(sb)->s_journal);
		next3_snapshot_set_active(sb, NULL);
		journal_unlock_updates(NEXT3_SB(sb)->s_journal);
		/* remove unused deleted active snapshot */
		next3_snapshot_remove(active_snapshot);
		/* drop the refcount to 0 */
		iput(active_snapshot);
	}
}

