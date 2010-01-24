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

#ifdef CONFIG_NEXT3_FS_SNAPSHOT_FILE_EXCLUDE
/*
 * Flag to prevent freeing old snapshot blocks while snapshot_clean() is
 * marking snapshot blocks in exclude bitmap.  It may only be set/cleared by
 * the owner of snapshot_mutex.
 *
 * snapshot_clean() sets it to snapshot ID to disable cleanup in
 * snapshot_update().
 * snapshot_take() sets it to -1 to abort a running snapshot_clean().
 * snapshot_clean() sets it to 0 before returning success or failure.
 */
static int cleaning;
#endif

/*
 * next3_snapshot_set_active() sets the current active snapshot to inode and
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
static int next3_snapshot_clean(handle_t *handle, struct inode *inode);
static void next3_snapshot_dump(struct inode *inode);

/*
 * next3_snapshot_get_flags() check snapshot state
 * Called from next3_ioctl() under i_mutex
 */
void next3_snapshot_get_flags(struct next3_inode_info *ei, struct file *filp)
{
#pragma ezk
#warning _get_flags fxn is only used in ioctl.c. can move it there and mk it static
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
#warning _set_flags fxn is only used in ioctl.c. can move it there and mk it static
	unsigned int oldflags = NEXT3_I(inode)->i_flags;
	int err = 0;

#ifdef CONFIG_NEXT3_FS_SNAPSHOT_DEBUG
	if (flags & NEXT3_SNAPFILE_DUMP_FL) {
#warning it seems ugly to have to save the current debug level, hardcode it to 5, then reset it.
#warning it migh be easier to pass the level you want to snapshot_dump and let it handle it there?
		int debug = snapshot_enable_debug;
		snapshot_enable_debug = 5;
		next3_snapshot_dump(inode);
		snapshot_enable_debug = debug;
		flags &= ~NEXT3_SNAPFILE_DUMP_FL;
	}
#endif

#ifdef CONFIG_NEXT3_FS_SNAPSHOT_FILE_EXCLUDE
	if ((flags & NEXT3_SNAPFILE_CLEAN_FL) &&
		!(oldflags & NEXT3_SNAPFILE_CLEAN_FL))
		err = next3_snapshot_clean(handle, inode);
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
 * next3_exclude_inode_bread():
 * Read indirect block from exclude inode.
 * If 'create' is true, try to allocate missing indirect block.
 *
 * Helper function for next3_snapshot_init_bitmap_cache().
 * Called under sb_lock and before snapshots are loaded, so changes made to
 * exclude inode are not COWed.
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

	snapshot_debug(1, "failed to read exclude "
			"inode indirect[%d] block\n",
			dind_offset);
	if (!create)
		return NULL;

	err = extend_or_restart_transaction(handle,
			NEXT3_RESERVE_TRANS_BLOCKS);
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
 * next3_exclude_inode_getblk():
 * Read location of exclude bitmap block from exclude inode.
 * If 'create' is true, try to allocate missing blocks.
 *
 * Helper function for next3_snapshot_init_bitmap_cache().
 * Called under sb_lock and before snapshots are loaded, so changes made to
 * exclude inode are not COWed.
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
	snapshot_debug(1, "failed to read exclude bitmap #%d block "
			"from exclude inode\n", grp);
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
				"bitmap #%d block\n", grp);
out:
	brelse(ind_bh);
	return exclude_bitmap;
}

/*
 * next3_snapshot_init_bitmap_cache():
 *
 * Init the COW/exclude bitmap cache for all block groups.
 * COW bitmap cache is set to 0 (lazy init on first access to block group).
 * Exclude bitmap blocks are read from exclude inode.
 * When mounting ext3 (!HAS_SNAPSHOT), try to create/resize exclude inode.
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
			       "disabling exclude bitmap!\n");
		return;
	}
	/* start large transaction that will be extended/restarted */
	handle = next3_journal_start(inode, NEXT3_MAX_TRANS_DATA);
	if (IS_ERR(handle))
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
 * next3_snapshot_create() initilizes a snapshot file
 * and adds it to the list of snapshots
 * Called under i_mutex and snapshot_mutex
 */
static int next3_snapshot_create(struct inode *inode)
{
	handle_t *handle;
	struct inode *snapshot = next3_snapshot_get_active(inode->i_sb);
	int i, count, err;
	struct next3_group_desc *desc;
	unsigned long ino;
	struct next3_iloc iloc;
	next3_fsblk_t bmap_blk = 0, imap_blk = 0, inode_blk = 0;
	next3_fsblk_t prev_inode_blk = 0;
	loff_t snapshot_blocks = le32_to_cpu(NEXT3_SB(inode->i_sb)->
					     s_es->s_blocks_count);
	static struct inode dummy_inode;
#ifdef CONFIG_NEXT3_FS_SNAPSHOT_LIST
	struct list_head *l, *list = &NEXT3_SB(inode->i_sb)->s_snapshot_list;

	if (!list_empty(list)) {
		struct inode *last_snapshot =
			&list_first_entry(list, struct next3_inode_info,
					  i_orphan)->vfs_inode;
		if (snapshot != last_snapshot) {
			snapshot_debug(1, "failed to add snapshot because last"
				       " snapshot (%u) is not active\n",
				       last_snapshot->i_generation);
			return -EPERM;
		}
	}
#else
	if (snapshot) {
		snapshot_debug(1, "failed to add snapshot because active "
			       "snapshot (%u) has to be deleted first\n",
			       snapshot->i_generation);
		return -EPERM;
	}
#endif

	/* verify that all inode's direct blocks are not allocated */
	for (i = 0; i <= NEXT3_TIND_BLOCK; i++) {
		if (NEXT3_I(inode)->i_data[i])
			break;
	}
	/* Don't need i_size_read because we hold i_mutex */
	if (i <= NEXT3_TIND_BLOCK || inode->i_size > 0 ||
	    NEXT3_I(inode)->i_disksize > 0) {
		snapshot_debug(1, "failed to create snapshot file (ino=%lu) "
			       "because it is not empty (i_data[%d], "
			       "i_size=%lld, i_disksize=%lld\n",
				inode->i_ino, i, inode->i_size,
			       NEXT3_I(inode)->i_disksize);
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
	NEXT3_I(inode)->i_flags |= (NEXT3_SNAPFILE_FL|NEXT3_SNAPFILE_TAKE_FL);
	NEXT3_I(inode)->i_flags &= ~NEXT3_SNAPFILE_ENABLED_FL;

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
		struct buffer_head *bh;
		/* meta blocks are journalled as dirty meta data */
		bh = next3_getblk(handle, inode, i, SNAPSHOT_WRITE, &err);
		if (bh) {
			err = next3_journal_get_write_access(handle, bh);
			if (!err) {
				/* zero out meta block */
				lock_buffer(bh);
				memset(bh->b_data, 0, bh->b_size);
				set_buffer_uptodate(bh);
				unlock_buffer(bh);
				err = next3_journal_dirty_metadata(handle, bh);
			}
			brelse(bh);
		}
		if (!bh || err) {
			snapshot_debug(1, "failed to initiate meta block (%d) "
				       "for snapshot (%u)\n",
					i, inode->i_generation);
			if (err >= 0)
				err = -EIO;
			goto out_handle;
		}
	}
	/* place pre-allocated [d,t]ind blocks in position */
	next3_snapshot_unhide(NEXT3_I(inode));

	/* allocate super block and group descriptors for snapshot */
	count = NEXT3_SB(inode->i_sb)->s_gdb_count + 1;
	err = count;
	for (i = 0; err > 0 && i < count; i += err) {
		err = extend_or_restart_transaction_inode(handle, inode,
				NEXT3_DATA_TRANS_BLOCKS(inode->i_sb));
		if (err)
			goto out_handle;
		err = next3_snapshot_map_blocks(handle, inode, i, count - i,
						NULL, SNAPSHOT_WRITE);
	}
	if (err <= 0) {
		snapshot_debug(1, "failed to allocate super block and %d "
			       "group descriptor blocks for snapshot (%u)\n",
			       count - 1, inode->i_generation);
		if (err)
			err = -EIO;
		goto out_handle;
	}

	/*
	 * start with journal inode and continue with snapshot list
	 * snapshot inode points to a dummy empty inode so
	 * journal inode direct block won't be alloacted
	 */
	snapshot = &dummy_inode;
	ino = NEXT3_JOURNAL_INO;
alloc_self_inode:
	/*
	 * pre-allocate for every snapshot inode including this one:
	 * - inode's group block and inode bitmap blocks
	 * - inode's own block
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
			NULL, SNAPSHOT_WRITE);
	count = err;
	/* allocate remaining blocks one by one */
	if (err > 0 && count < 2)
		err = next3_snapshot_map_blocks(handle, inode,
				imap_blk, 1,
				NULL,
				SNAPSHOT_WRITE);
	if (err > 0 && count < 3)
		err = next3_snapshot_map_blocks(handle, inode,
				inode_blk, 1,
				NULL,
				SNAPSHOT_WRITE);
next_snapshot:
	if (!bmap_blk || !imap_blk || !inode_blk || err < 0) {
		next3_fsblk_t blk0 = iloc.block_group *
			NEXT3_BLOCKS_PER_GROUP(inode->i_sb);
		snapshot_debug(1, "failed to allocate self inode and "
				"bitmap blocks of snapshot (%u) "
				"(%lu,%lu,%lu/%lu) for snapshot (%u)\n",
				snapshot->i_generation, bmap_blk - blk0,
				imap_blk - blk0, inode_blk - blk0,
				iloc.block_group, inode->i_generation);
		if (!err)
			err = -EIO;
		goto out_handle;
	}
#ifdef CONFIG_NEXT3_FS_SNAPSHOT_LIST
	if (l != list) {
		snapshot = &list_entry(l, struct next3_inode_info,
				       i_orphan)->vfs_inode;
		ino = snapshot->i_ino;
		l = l->next;
		goto alloc_self_inode;
	}
#endif

	/* sleep 1 tunable delay unit */
	snapshot_test_delay(SNAPTEST_TAKE);
	snapshot_debug(1, "snapshot (%u) created\n", inode->i_generation);
	err = 0;
out_handle:
	next3_journal_stop(handle);
	if (err)
		NEXT3_I(inode)->i_flags &=
			~(NEXT3_SNAPFILE_FL|NEXT3_SNAPFILE_TAKE_FL);
	return err;
}

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
	/*
	 * If we call next3_getblk() with NULL handle
	 * we will get read through access to snapshot inode.
	 * We don't want read through access in snapshot_take(),
	 * so we call next3_getblk() with this dummy handle.
	 * Because we are not allocating snapshot block here
	 * the handle will not be used anyway.
	 */
	static handle_t dummy_handle;
#ifdef CONFIG_NEXT3_FS_SNAPSHOT_LIST
	struct list_head *list = &NEXT3_SB(inode->i_sb)->s_snapshot_list;
	struct list_head *l = list->next;
#endif
	next3_fsblk_t prev_inode_blk = 0;
	struct inode *snapshot;
	struct super_block *sb = inode->i_sb;
	struct next3_sb_info *sbi = NEXT3_SB(sb);
	struct buffer_head *sbh = NULL;
#ifdef CONFIG_NEXT3_FS_SNAPSHOT_EXCLUDE_BITMAP
	struct buffer_head *exclude_bitmap_bh = NULL;
#endif
	struct buffer_head *bhs[3];
	const char *strs[3];
	int nbhs = 0;
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
				   SNAPSHOT_READ, &err);

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

#ifdef CONFIG_NEXT3_FS_SNAPSHOT_FILE_EXCLUDE
	/*
	 * abort running clean of active snapshot
	 * freeze_fs() below will wait until snapshot_clean()
	 * has aborted and closed it's transaction
	 */
	if (cleaning > 0)
		cleaning = -1;
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
	 * Remove the last snapshot inode number but keep the
	 * has_snapshot flag, to force read-only mount and prevent
	 * fsck from clearing exclude bitmap.
	 */
	es->s_feature_compat &= ~cpu_to_le32(NEXT3_FEATURE_COMPAT_HAS_JOURNAL);
	es->s_journal_inum = 0;
	es->s_last_snapshot = 0;
	set_buffer_uptodate(sbh);
	unlock_buffer(sbh);
	mark_buffer_dirty(sbh);
	/*
	 * copy group descriptors to snapshot
	 */
	for (i = 0; i < sbi->s_gdb_count; i++) {
		struct buffer_head *bh = sbi->s_group_desc[i];
		long iblock = SNAPSHOT_IBLOCK(bh->b_blocknr);

		brelse(sbh);
		sbh = next3_getblk(&dummy_handle, inode, iblock,
				SNAPSHOT_READ, &err);
		if (!sbh || sbh->b_blocknr == bh->b_blocknr) {
			snapshot_debug(1, "warning: GDT block (%lld) of "
				       "snapshot (%u) not allocated\n",
				       bh->b_blocknr, inode->i_generation);
			err = -EIO;
			goto out_unlockfs;
		}
		err = next3_snapshot_copy_buffer(sbh, bh, NULL);
		if (err)
			goto out_unlockfs;
	}
	/*
	 * copy self inode and bitmap blocks to snapshot
	 * start with journal inode and continue with snapshot list
	 */
	snapshot = sbi->s_journal_inode;
	/* XXX: who cares about generation of the journal inode? */
	snapshot->i_generation = 0;
copy_self_inode:
	while (nbhs > 0)
		brelse(bhs[--nbhs]);
	err = next3_get_inode_loc(snapshot, &iloc);
	if (!err)
		desc = next3_get_group_desc(sb, iloc.block_group, NULL);
	if (err || !iloc.bh || !iloc.bh->b_blocknr || !desc) {
		snapshot_debug(1, "failed to get self inode and bitmap blocks "
			       "of snapshot (%u)\n", snapshot->i_generation);
		goto out_unlockfs;
	}
	if (iloc.bh->b_blocknr == prev_inode_blk)
		goto fix_self_inode;
	prev_inode_blk = iloc.bh->b_blocknr;
	/* keep block bitmap buffer at index 0 and iloc.bh last */
	strs[nbhs] = "self block bitmap";
	bhs[nbhs] = sb_bread(sb, le32_to_cpu(desc->bg_block_bitmap));
	nbhs++;
	strs[nbhs] = "self inode bitmap";
	bhs[nbhs] = sb_bread(sb, le32_to_cpu(desc->bg_inode_bitmap));
	nbhs++;
	strs[nbhs] = "self inode";
	bhs[nbhs] = iloc.bh;
	nbhs++;
	for (i = 0; i < nbhs; i++) {
		struct buffer_head *bh = bhs[i];
		const char *mask = NULL;
#ifdef CONFIG_NEXT3_FS_SNAPSHOT_EXCLUDE_BITMAP
		if (i == 0) {
			brelse(exclude_bitmap_bh);
			exclude_bitmap_bh = read_exclude_bitmap(sb,
							iloc.block_group);
			if (exclude_bitmap_bh)
				mask = exclude_bitmap_bh->b_data;
		}
#endif
		if (bh) {
			brelse(sbh);
			sbh = next3_getblk(&dummy_handle, inode,
					   SNAPSHOT_IBLOCK(bh->b_blocknr),
					   SNAPSHOT_READ, &err);
		}
		if (!bh || err || !sbh || sbh->b_blocknr == bh->b_blocknr) {
			snapshot_debug(1, "failed to copy snapshot (%u) %s "
			       "block [%lld/%lld] to snapshot (%u)\n",
			       snapshot->i_generation, strs[i],
			       SNAPSHOT_BLOCK_GROUP_OFFSET(bh->b_blocknr),
			       SNAPSHOT_BLOCK_GROUP(bh->b_blocknr),
			       inode->i_generation);
			if (!err)
				err = -EIO;
			goto out_unlockfs;
		}
		err = next3_snapshot_copy_buffer(sbh, bh, mask);
		if (err)
			goto out_unlockfs;
		snapshot_debug(4, "copied snapshot (%u) %s block [%lld/%lld] "
			       "to snapshot (%u)\n", snapshot->i_generation,
			       strs[i],
			       SNAPSHOT_BLOCK_GROUP_OFFSET(bh->b_blocknr),
			       SNAPSHOT_BLOCK_GROUP(bh->b_blocknr),
			       inode->i_generation);
	}
fix_self_inode:
	/* get snapshot copy of raw inode */
	iloc.bh = sbh;
	raw_inode = next3_raw_inode(&iloc);
	if (snapshot->i_ino == NEXT3_JOURNAL_INO) {
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
#ifdef CONFIG_NEXT3_FS_SNAPSHOT_LIST
	if (l != list) {
		snapshot = &list_entry(l, struct next3_inode_info,
				       i_orphan)->vfs_inode;
		l = l->next;
		goto copy_self_inode;
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
	/* set as active snapshot in-memory */
	NEXT3_I(inode)->i_flags &= ~NEXT3_SNAPFILE_TAKE_FL;
	next3_snapshot_set_active(sb, inode);
	/* set snapshot file read-only aops */
	next3_set_aops(inode);
	/* reset COW bitmap cache */
	next3_snapshot_reset_bitmap_cache(sb, 0);

out_unlockfs:
	/*
	 * Sync all copied snapshot blocks.
	 * No need to sync the snapshot inode to disk because all
	 * that has changed are the snapshot flags TAKE and ACTIVE
	 * which are fixed on load by snapshot_update().
	 */
	filemap_fdatawrite(inode->i_mapping);
	unlock_super(sb);
	sb->s_op->unfreeze_fs(sb);

	if (err)
		goto out_err;

#ifdef CONFIG_NEXT3_FS_SNAPSHOT_DEBUG
#warning instead of this 'if' below, maybe have snapshot_dump do the check?
	if (snapshot_enable_debug >= 5)
		next3_snapshot_dump(inode);
#endif
	/* sleep 1 tunable delay unit */
	snapshot_test_delay(SNAPTEST_TAKE);
	snapshot_debug(1, "snapshot (%u) has been taken\n",
			inode->i_generation);

out_err:
#ifdef CONFIG_NEXT3_FS_SNAPSHOT_EXCLUDE_BITMAP
	brelse(exclude_bitmap_bh);
#endif
	brelse(sbh);
	while (nbhs > 0)
		brelse(bhs[--nbhs]);
	return err;
}

#ifdef CONFIG_NEXT3_FS_SNAPSHOT_FILE_EXCLUDE
/*
 * next3_snapshot_clean() marks old snapshots blocks in exclude bitmap.
 * This function is for sanity tests and disaster recovery only.
 * If everything works properly, all snapshot blocks should already
 * be set in exclude bitmap.
 *
 * Q: snapshot blocks are not COWed on snapshot shrink/merge/cleanup,
 *    so why is the exclude bitmap needed?
 *
 * A: the following example explains the problem of merging non-clean
 *    snapshots:
 *
 * 1. create a 100M file 'foo' in the root directory - allocated from block
 *     group 0
 *
 * 2. take snapshot 1
 * 3. remove 'foo' - snapshot 1 now contains all 'foo' blocks
 * 4. take snapshot 2 and snapshot 3
 * 5. delete snapshot 1 - 'foo' blocks are excluded from snapshot 3 and freed
 * 6. create a 100M file 'bar' in the root directory - 'foo' blocks are
 *    reallocated.
 * 7. take snapshot 4
 * 8. remove 'bar' - snapshot 4 now contains all 'bar' blocks
 * 9. take snapshot 5
 * 10. delete snapshot 3
 * 11. delete snapshot 4 - merge moves 'bar' blocks into snapshot 2
 *
 * At the end of this long example, snapshot 2 disk usage is ~100M, while it
 * does not actually contain any reference to such a large file.  It contains
 * a reference to snapshot 1, which was 100M, but should have been excluded
 * from snapshot 2, but it wasn't, because snapshot 1 was deleted when
 * snapshot 3 was active.
 *
 * To solve this problem, we must clean snapshot 2 before taking snapshot 3.
 * It is not mandatory to clean a snapshot after take, but merge will not
 * move any blocks into a non-clean snapshot, so the disk space in the
 * example above cannot be reclaimed until snapshot 2 is deleted.
 *
 * Called under i_mutex and snapshot_mutex, drops them after setting the
 * 'cleaning' flag and reacquires them before returning.
 */
static int next3_snapshot_clean(handle_t *handle, struct inode *inode)
{
	struct list_head *l;
	struct inode *snapshot = next3_snapshot_get_active(inode->i_sb);
	struct next3_inode_info *ei;
	struct buffer_head *bh = NULL;
	__le32 nr;
	int i, err;

	if (!(NEXT3_I(inode)->i_flags & NEXT3_SNAPFILE_FL)) {
		snapshot_debug(1, "next3_snapshot_clean() called with non "
			       "snapshot file (ino=%lu)\n", inode->i_ino);
		return -EPERM;
	}

	if (snapshot != inode) {
		snapshot_debug(1, "failed to clean non active snapshot (%u)\n",
				inode->i_generation);
		return -EPERM;
	}

	if (cleaning) {
		snapshot_debug(1, "failed to clean snapshot (%u)"
				" because snapshot (%d) is being cleaned\n",
				inode->i_generation, cleaning);
		return -EPERM;
	}

	/* set the 'cleaning' flag and release snapshot_mutex and inode_mutex */
	cleaning = inode->i_generation;
	mutex_unlock(&NEXT3_SB(inode->i_sb)->s_snapshot_mutex);
	mutex_unlock(&inode->i_mutex);

	/* iterate from oldest snapshot backwards */
	list_for_each_prev(l, &NEXT3_SB(inode->i_sb)->s_snapshot_list) {
		ei = list_entry(l, struct next3_inode_info, i_orphan);
		snapshot = &ei->vfs_inode;
		if (snapshot == inode)
			/* no need to clear snapshot blocks from its own COW
			 * bitmap */
			break;
		if (cleaning < 0)
			/* clean of active snapshot aborted by
			 * snapshot_take() */
			break;

		/* exclude the snapshot meta blocks */
		for (i = 0; i <= SNAPSHOT_META_BLOCKS; i++) {
			nr = ei->i_data[i];
			if (nr)
				next3_snapshot_get_clear_access(
						handle, snapshot,
						le32_to_cpu(nr), 1);
		}
		/* exclude the snapshot dind branch */
		nr = ei->i_data[NEXT3_DIND_BLOCK];
		if (nr) {
			next3_free_branches_cow(handle, snapshot, NULL,
					&nr, &nr+1, 2, 1);
			ei->i_data[NEXT3_DIND_BLOCK] = 0;
		}
		/* exclude the snapshot tind branch */
		nr = ei->i_data[NEXT3_TIND_BLOCK];
		if (nr) {
			next3_free_branches_cow(handle, snapshot, NULL,
					&nr, &nr+1, 3, 1);
			ei->i_data[NEXT3_TIND_BLOCK] = 0;
		}
		snapshot_debug(2, "snapshot (%u) cleaned snapshot (%u) "
				"blocks\n",
				inode->i_generation,
				snapshot->i_generation);
	}

	/*
	 * snapshot_mutex may be taken by snapshot_take(), which is trying
	 * to abort this clean and is waiting for this transaction to close,
	 * so unless we restart the transaction here we would be
	 * dead-locked.
	 */
	err = next3_journal_restart(handle, 1);
	/* retake snapshot_mutex and inode_mutex and clear the 'cleaning'
	 * flag */
	mutex_lock(&inode->i_mutex);
	mutex_lock(&NEXT3_SB(inode->i_sb)->s_snapshot_mutex);
	if (cleaning < 0) {
		/* clean of active snapshot aborted by snapshot_take() */
		snapshot_debug(1, "snapshot (%u) clean interrupted by "
			       "snapshot take\n", inode->i_generation);
		err = -EINTR;
	} else if (err) {
		snapshot_debug(1, "failed to clean active snapshot (%u) "
			       "(err=%d)\n", inode->i_generation, err);
	} else {
		NEXT3_I(inode)->i_flags |= NEXT3_SNAPFILE_CLEAN_FL;
		snapshot_debug(1, "snapshot (%u) is clean\n",
			       inode->i_generation);
	}
	cleaning = 0;
	brelse(bh);
	return err;
}
#endif

/*
 * next3_snapshot_enable() enables snapshot mount
 * sets the in-use flag and the active snapshot
 * Called under i_mutex and snapshot_mutex
 */
static int next3_snapshot_enable(struct inode *inode)
{
	if (!(NEXT3_I(inode)->i_flags & NEXT3_SNAPFILE_FL)) {
		snapshot_debug(1, "next3_snapshot_enable() called with non "
			       "snapshot file (ino=%lu)\n",
			       inode->i_ino);
		return -EPERM;
	}

	if (NEXT3_I(inode)->i_flags &
	    (NEXT3_SNAPFILE_DELETED_FL|NEXT3_SNAPFILE_TAKE_FL)) {
		snapshot_debug(1, "failed to enable snapshot (%u) "
			       "(deleted|take)\n", inode->i_generation);
		return -EPERM;
	}

	/*
	 * set i_size to block device size to enable loop device mount
	 */
	SNAPSHOT_SET_ENABLED(inode);
	NEXT3_I(inode)->i_flags |= NEXT3_SNAPFILE_ENABLED_FL;

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
	if (!(NEXT3_I(inode)->i_flags & NEXT3_SNAPFILE_FL)) {
		snapshot_debug(1, "next3_snapshot_disable() called with non "
			       "snapshot file (ino=%lu)\n", inode->i_ino);
		return -EPERM;
	}

	if (NEXT3_I(inode)->i_flags & NEXT3_SNAPFILE_OPEN_FL) {
		snapshot_debug(1, "failed to disable mounted snapshot (%u)\n",
				inode->i_generation);
		return -EPERM;
	}

	/*
	 * set i_size to zero to disable loop device mount
	 */
	SNAPSHOT_SET_DISABLED(inode);
	NEXT3_I(inode)->i_flags &= ~NEXT3_SNAPFILE_ENABLED_FL;

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
	if (NEXT3_I(inode)->i_flags & NEXT3_SNAPFILE_ENABLED_FL) {
		snapshot_debug(1, "failed to delete enabled snapshot (%u)\n",
				inode->i_generation);
		return -EPERM;
	}

	/* mark deleted for later cleanup to finish the job */
	NEXT3_I(inode)->i_flags |= NEXT3_SNAPFILE_DELETED_FL;
	snapshot_debug(1, "snapshot (%u) marked for deletion\n",
			inode->i_generation);
	return 0;
}
#endif

/*
 * next3_snapshot_cleanup() removes a snapshot inode from the list
 * of snapshots stored on disk and truncates the snapshot inode
 * Called from next3_snapshot_update() under snapshot_mutex
 */
static int next3_snapshot_cleanup(struct inode *inode)
{
	handle_t *handle;
	struct next3_sb_info *sbi;
	struct next3_inode_info *ei = NEXT3_I(inode);
	__le32 nr = 0;
	int err = 0;

	/* elevate ref count until final cleanup */
	if (!igrab(inode))
		return 0;

	if (ei->i_flags & (NEXT3_SNAPFILE_ENABLED_FL | NEXT3_SNAPFILE_INUSE_FL
			   | NEXT3_SNAPFILE_ACTIVE_FL)) {
		snapshot_debug(4, "deferred delete of snapshot (%u) "
			       "(inuse|enabled|active)\n", inode->i_generation);
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
	/*
	 * a very simplified version of next3_truncate()
	 * we free the snapshot blocks here, to make sure
	 * they are excluded from the active snapshot.
	 * there is no need to use the orphan list because
	 * snapshot is under the protection of the snapshot list
	 */
	nr = ei->i_data[NEXT3_DIND_BLOCK];
	if (nr) {
		next3_free_branches(handle, inode, NULL, &nr, &nr+1, 2);
		ei->i_data[NEXT3_DIND_BLOCK] = 0;
	}
	nr = ei->i_data[NEXT3_TIND_BLOCK];
	if (nr) {
		next3_free_branches(handle, inode, NULL, &nr, &nr+1, 3);
		ei->i_data[NEXT3_TIND_BLOCK] = 0;
	}
	/* only snapshot meta blocks are left after the truncate */
	SNAPSHOT_SET_DISABLED(inode);
	ei->i_disksize = SNAPSHOT_META_SIZE;
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
	 * but it may have the SNAPSFILE_DELETED flag set.
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
 * next3_snapshot_shrink() frees unused blocks from
 * deleted snapshot files.
 * 'start' is the latest non-deleted snapshot before
 * the deleted snapshots, which are due for shrinking.
 * 'end' is the first non-deleted (or active) snapshot after
 * the deleted snapshots, which are due for shrinking.
 * 'need_shrink' is the number of deleted snapshot files
 * between 'start' and 'end' which are due for shrinking.
 * Called from next3_snapshot_update() under snapshot_mutex
 */
static int next3_snapshot_shrink(struct inode *start, struct inode *end,
				 int need_shrink)
{
	struct list_head *l;
	handle_t *handle;
	struct buffer_head cow_bitmap, *cow_bh;
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
			cow_bitmap.b_blocknr = -1000;
			cow_bh = &cow_bitmap;
			bg_boundary += SNAPSHOT_BLOCKS_PER_GROUP;
			block_group++;
			if (block >= snapshot_blocks)
				/*
				 * Past last snapshot block group - pass NULL
				 * cow_bh to next3_snapshot_shrink_blocks().
				 * This will cause snapshots after resize to
				 * shrink to start snapshot size.
				 */
				cow_bh = NULL;
		}

		err = extend_or_restart_transaction(handle,
						    NEXT3_MAX_TRANS_DATA);
		if (err)
			goto out_err;

		err = next3_snapshot_shrink_blocks(handle, start, end,
					      SNAPSHOT_IBLOCK(block), count,
					      &cow_bitmap);

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
	/* iterate from 'start' to 'end' */
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
 * next3_snapshot_merge() moves blocks from shrunk snapshot files.
 * 'start' is the latest non-deleted snapshot before
 * the shrunk snapshots, which are due for merging.
 * 'end' is the first non-deleted (or active) snapshot after
 * the shrunk snapshots, which are due for merging.
 * 'need_merge' is the number of shrunk snapshot files
 * between 'start' and 'end' which are due for merging.
 * Called from next3_snapshot_update() under snapshot_mutex
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

	/* iterate safe from 'start' to 'end' */
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
		next3_snapshot_cleanup(inode);

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
 * Snapshot constructor/destructor
 */

/*
 * next3_snapshot_load() loads the in-memoery snapshot list from disk
 * Called from next3_fill_super() under sb_lock
 */
void next3_snapshot_load(struct super_block *sb, struct next3_super_block *es)
{
	__le32 *ino_next = &es->s_last_snapshot;

#ifdef CONFIG_NEXT3_FS_SNAPSHOT_LIST
	if (!list_empty(&NEXT3_SB(sb)->s_snapshot_list)) {
		snapshot_debug(1, "warning: snaphots already loaded!\n");
		return;
	}
#endif

#ifdef CONFIG_NEXT3_FS_SNAPSHOT_JOURNAL_CREDITS
	if (!NEXT3_HAS_COMPAT_FEATURE(sb,
		NEXT3_FEATURE_COMPAT_BIG_JOURNAL))
		snapshot_debug(1, "warning: big_journal feature is not set - "
			       "this might affect concurrnet filesystem "
			       "writers performance!\n");
#endif

	/* init COW bitmap and exclude bitmap cache */
	next3_snapshot_init_bitmap_cache(sb);

	while (*ino_next) {
		struct inode *inode;

		inode = next3_orphan_get(sb, le32_to_cpu(*ino_next));
		if (IS_ERR(inode)) {
			snapshot_debug(1, "warning: found bad inode (%u) in "
				       "snapshots list!\n",
				       le32_to_cpu(*ino_next));
			*ino_next = 0;
			break;
		}

		if (!(NEXT3_I(inode)->i_flags & NEXT3_FL_SNAPSHOT_MASK)) {
			snapshot_debug(1, "warning: non snapshot file (%u) in "
				       "snapshots list!\n",
				       le32_to_cpu(*ino_next));
			*ino_next = 0;
			iput(inode);
			break;
		}

#ifdef CONFIG_NEXT3_FS_SNAPSHOT_DEBUG
#warning instead of this 'if' below, maybe have snapshot_dump do the check?
		if (snapshot_enable_debug >= 5)
			next3_snapshot_dump(inode);
#endif
		snapshot_debug(1, "snapshot (%u) loaded\n",
			       inode->i_generation);

		if (!NEXT3_HAS_RO_COMPAT_FEATURE(sb,
			NEXT3_FEATURE_RO_COMPAT_HAS_SNAPSHOT)) {
			/* fix missing has_snapshot flag */
			NEXT3_SET_RO_COMPAT_FEATURE(sb,
				    NEXT3_FEATURE_RO_COMPAT_HAS_SNAPSHOT);
			snapshot_debug(1, "warning: fixing has_snapshot "
				       "flag!\n");
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

	next3_snapshot_update(sb, 0);
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
	next3_snapshot_set_active(sb, NULL);
}

/*
 * next3_snapshot_update() updates snapshots status
 * if 'cleanup' is true, shrink/merge/cleanup
 * all snapshots marked for deletion
 * Called under snapshot_mutex or sb_lock
 */
void next3_snapshot_update(struct super_block *sb, int cleanup)
{
	struct inode *active_snapshot;
	struct inode *used_by = NULL; /* last non-deleted snapshot found */
#ifdef CONFIG_NEXT3_FS_SNAPSHOT_LIST
	struct list_head *l, *n;
	struct inode *inode;
	struct next3_inode_info *ei;
	int found_active = 0;
	int found_enabled = 0;
#ifdef CONFIG_NEXT3_FS_SNAPSHOT_FILE_SHRINK
	int need_shrink = 0;
#endif
#ifdef CONFIG_NEXT3_FS_SNAPSHOT_FILE_MERGE
	int need_merge = 0;
#endif
#endif

#ifdef CONFIG_NEXT3_FS_SNAPSHOT_FILE_EXCLUDE
	if (cleaning)
		/*
		 * don't free old snapshot blocks while snapshot_clean()
		 * is clearing snapshot blocks from COW bitmap
		 */
		cleanup = 0;
#endif

	active_snapshot = next3_snapshot_get_active(sb);
	if (!active_snapshot)
		return;

#ifdef CONFIG_NEXT3_FS_SNAPSHOT_LIST
	/* iterate safe from oldest snapshot backwards */
	list_for_each_prev_safe(l, n, &NEXT3_SB(sb)->s_snapshot_list) {
		ei = list_entry(l, struct next3_inode_info, i_orphan);
		inode = &ei->vfs_inode;
		/* all snapshots on the list must have the SNAPSHOT flag and
		 * are not zombies */
		ei->i_flags |= NEXT3_SNAPFILE_FL;
		ei->i_flags &= ~NEXT3_SNAPFILE_ZOMBIE_FL;

		/* snapshot later than active (failed take) should be
		 * deleted */
		if (found_active || (ei->i_flags & NEXT3_SNAPFILE_TAKE_FL))
			ei->i_flags |= NEXT3_SNAPFILE_TAKE_FL |
				NEXT3_SNAPFILE_DELETED_FL;

		/*
		 * after completion of a snapshot management operation,
		 * only the active snapshot can have the ACTIVE flag
		 */
		if (inode == active_snapshot) {
			ei->i_flags |= NEXT3_SNAPFILE_ACTIVE_FL;
			found_active = 1;
		} else
			ei->i_flags &= ~NEXT3_SNAPFILE_ACTIVE_FL;

		if (found_enabled && !(ei->i_flags & NEXT3_SNAPFILE_TAKE_FL))
			/* snapshot is in use by an older enabled snapshot */
			ei->i_flags |= NEXT3_SNAPFILE_INUSE_FL;
		else
			/* snapshot is not in use by older enabled snapshots */
			ei->i_flags &= ~NEXT3_SNAPFILE_INUSE_FL;

		if ((ei->i_flags & NEXT3_SNAPFILE_DELETED_FL) &&
			!(ei->i_flags & NEXT3_SNAPFILE_ACTIVE_FL)) {
			/* deleted (non-active) snapshot file */
			if ((cleanup && !used_by) ||
			    (ei->i_flags & NEXT3_SNAPFILE_TAKE_FL))
				/* cleanup permanently unused deleted
				 * snapshot */
				next3_snapshot_cleanup(inode);
			else if (cleanup) {
#ifdef CONFIG_NEXT3_FS_SNAPSHOT_FILE_SHRINK
				if (!(ei->i_flags & NEXT3_SNAPFILE_SHRUNK_FL))
					/* deleted snapshot needs shrinking */
					need_shrink++;
#endif
#ifdef CONFIG_NEXT3_FS_SNAPSHOT_FILE_MERGE
				if (!found_enabled)
					/* temporarily unused deleted
					 * snapshot needs merging */
					need_merge++;
#endif
			}
		} else {
			/* non-deleted (or active) snapshot file */
#ifdef CONFIG_NEXT3_FS_SNAPSHOT_FILE_SHRINK
			if (cleanup && need_shrink)
				/* pass 1: shrink all deleted snapshots
				 * between 'used_by' and 'inode' */
				next3_snapshot_shrink(used_by, inode,
						      need_shrink);
#endif
#ifdef CONFIG_NEXT3_FS_SNAPSHOT_FILE_MERGE
			if (cleanup && need_merge)
				/* pass 2: merge all shrunk snapshots
				 * between 'used_by' and 'inode' */
				next3_snapshot_merge(used_by, inode,
						     need_merge);
#endif
			need_shrink = 0;
			need_merge = 0;
			if (!found_active)
				/* newer snapshot are potentialy used by
				 * this snapshot (when it is enabled) */
				used_by = inode;
			if (ei->i_flags & NEXT3_SNAPFILE_ENABLED_FL)
				found_enabled = 1;
		}
	}
#endif

	/* if active snapshot is permanently unused and deleted, deactivate
	   it */
	if (cleanup && !used_by &&
		(NEXT3_I(active_snapshot)->i_flags & NEXT3_SNAPFILE_DELETED_FL)
	    && igrab(active_snapshot)) {
		/* lock journal updates before deactivating snapshot */
		journal_lock_updates(NEXT3_SB(sb)->s_journal);
		next3_snapshot_set_active(sb, NULL);
		journal_unlock_updates(NEXT3_SB(sb)->s_journal);
		/* cleanup unused deleted active snapshot */
		next3_snapshot_cleanup(active_snapshot);
		/* drop the refcount to 0 */
		iput(active_snapshot);
	}
}

#ifdef CONFIG_NEXT3_FS_SNAPSHOT_DEBUG
#warning you are defining this struct twice in snapshot_ctl.c and inode.c, slightly differently each time (just order of fields differs). Plz define only once if you can, else use two different structures and explain why two similar ones are needed.
struct Indirect {
	struct buffer_head *bh;
	__le32	*p;
	__le32	key;
};

#warning add comment at top of fxn below. logic of fxn unclear. plz explain
static void next3_snapshot_dump_ind(struct Indirect *ind,
		int i, int n, int l,
		int *pnmoved, int *pncopied)
{
#pragma ezk
	__le32 blk, prev_key;
	int j, b, k = 0;

	snapshot_debug_l(5, l-1, "{\n");

	ind->key = 0;
	for (j = 0; j <= SNAPSHOT_ADDR_PER_BLOCK; j++, ind->p++) {
		prev_key = ind->key;
		blk = (n << 2*SNAPSHOT_ADDR_PER_BLOCK_BITS) +
			(i << SNAPSHOT_ADDR_PER_BLOCK_BITS) + j;
		ind->key = (j < SNAPSHOT_ADDR_PER_BLOCK) ?
			le32_to_cpu(*(ind->p)) : 0;
		if (prev_key && (ind->key == prev_key ||
				 ind->key == prev_key+1))
			k++;
		else
			k = -k;

		if (!prev_key || k > 0)
			goto keep_counting;

		b = ((i % SNAPSHOT_IND_PER_BLOCK_GROUP)
		     << SNAPSHOT_ADDR_PER_BLOCK_BITS) + j - 1;
		if (prev_key == blk - 1)
			snapshot_debug_l(5, l,
					"block[%d-%d/%d]\n", b+k, b,
					n * SNAPSHOT_IND_PER_BLOCK_GROUP +
					i / SNAPSHOT_IND_PER_BLOCK_GROUP);
		else if (k <= 0)
			snapshot_debug_l(5, l, "block[%d-%d/%d]"
					" = [%u-%u/%u]\n", b+k, b,
					n * SNAPSHOT_IND_PER_BLOCK_GROUP +
					i / SNAPSHOT_IND_PER_BLOCK_GROUP,
					SNAPSHOT_BLOCK_GROUP_OFFSET(prev_key)+k,
					SNAPSHOT_BLOCK_GROUP_OFFSET(prev_key),
					SNAPSHOT_BLOCK_GROUP(prev_key));
		k = 0;

keep_counting:
		if (!ind->key)
			continue;
		else if (ind->key == blk)
			(*pnmoved)++;
		else
			(*pncopied)++;
	}

	snapshot_debug_l(5, l-1, "}\n");
}

#warning the two snapshot_dump functions are DBEUG only. shouldnt they live in debug.c?
/*
 * next3_snapshot_dump() prints a snapshot inode block map
 * Called under i_mutex or sb_lock
 */
#warning if this can be called under one of two locks, this could lead to odd races b/t threads which ones one lock vs. the other.
static void next3_snapshot_dump(struct inode *inode)
{
#pragma ezk
	struct next3_inode_info *ei = NEXT3_I(inode);
	struct Indirect chain[4] = {{NULL}, {NULL} }, *ind = chain;
	int nmeta = 0, nind = 0, ncopied = 0, nmoved = 0;
	int n = 0, i, l = 0;
#warning I feel this fxn is a bit hard to follow, esp. with all of the integers you increment and decremenat all over.  can it be simplified?
	/* print double indirect block map */
	snapshot_debug(5, "snapshot (%u) block map:\n", inode->i_generation);
	for (i = 0; i < SNAPSHOT_META_BLOCKS; i++) {
		if (ei->i_data[i]) {
			ind->key = le32_to_cpu(ei->i_data[i]);
			nmeta++;
			snapshot_debug_l(5, l, "meta[%d] = [%u/%u]\n", i,
					SNAPSHOT_BLOCK_GROUP_OFFSET(ind->key),
					SNAPSHOT_BLOCK_GROUP(ind->key));
		}
	}
	ind->key = le32_to_cpu(ei->i_data[NEXT3_DIND_BLOCK]);
	if (!ind->key)
		goto ind_out;
dump_dind:
	brelse(ind->bh);
	ind->bh = sb_bread(inode->i_sb, ind->key);
	if (!ind->bh)
		goto ind_out;
	snapshot_debug_l(5, l, "dind[%d] = [%u/%u]\n", n,
			SNAPSHOT_BLOCK_GROUP_OFFSET(ind->key),
			SNAPSHOT_BLOCK_GROUP(ind->key));
	(ind+1)->p = (__le32 *)ind->bh->b_data;
	ind++;
	nind++;
	snapshot_debug_l(5, l, "{\n");
	l++;
	for (i = 0; i < SNAPSHOT_ADDR_PER_BLOCK; i++, ind->p++) {
		ind->key = le32_to_cpu(*(ind->p));
		if (!ind->key)
			continue;
		brelse(ind->bh);
		ind->bh = sb_bread(inode->i_sb, ind->key);
		if (!ind->bh)
			goto ind_out;
		snapshot_debug_l(5, l, "ind[%d] = [%u/%u]\n", i,
				SNAPSHOT_BLOCK_GROUP_OFFSET(ind->key),
				SNAPSHOT_BLOCK_GROUP(ind->key));
#warning u can swap next two lines and change next one to ind->p = ...
		(ind+1)->p = (__le32 *)ind->bh->b_data;
		ind++;
		nind++;
#warning why increment ind then decrement it after calling dump_ind; why not just pass ind+1 to it?
		next3_snapshot_dump_ind(ind, i, n, l+1, &nmoved, &ncopied);
		ind--;
	}
	l--;
	snapshot_debug_l(5, l, "}\n");
	ind--;

	if (ind == chain) {
#warning where is the actual printing of the triple indirect maps? does it happen because you goto dump_dind on each triple indirect block?  maybe this function can be broken into several more helpers to avoid this nasty gotos up and down.
#warning lkml wont like code that interleaves gotos going in two opposite directions. big no-no
		/* print triple indirect map */
		ind->key = le32_to_cpu(ei->i_data[NEXT3_TIND_BLOCK]);
		if (!ind->key)
			goto ind_out;
		brelse(ind->bh);
		ind->bh = sb_bread(inode->i_sb, ind->key);
		if (!ind->bh)
			goto ind_out;
		snapshot_debug_l(5, l, "tind[0] = [%u/%u]\n",
				SNAPSHOT_BLOCK_GROUP_OFFSET(ind->key),
				SNAPSHOT_BLOCK_GROUP(ind->key));
#warning u can swap next two lines and change next one to ind->p = ...
		(ind+1)->p = (__le32 *)ind->bh->b_data;
		ind++;
		nind++;
		snapshot_debug_l(5, l, "{\n");
		l++;
	}

	if (ind > chain) {
#warning i think u can safely eliminate the "n" variable and reuse "i" below.
		while (n++ < SNAPSHOT_ADDR_PER_BLOCK) {
			ind->key = le32_to_cpu(*(ind->p++));
			if (ind->key)
				goto dump_dind;
		}
	}

	l--;
	snapshot_debug_l(5, l, "}\n");
	ind--;
ind_out:
#warning can you rename "i" below to something more descriptive, like "total_blocks"?
	i = nmeta + nind + ncopied + nmoved;
	snapshot_debug(5, "snapshot (%u) contains: %d (meta) + %d (indirect) "
		       "+ %d (data) = %d blocks = %dK = %dM\n",
		       inode->i_generation, nmeta, nind, ncopied + nmoved,
		       i, i << (SNAPSHOT_BLOCK_SIZE_BITS - 10),
		       i >> (20 - SNAPSHOT_BLOCK_SIZE_BITS));
	snapshot_debug(5, "snapshot (%u) maps: %d (copied) + %d (moved) = "
		       "%d blocks\n",
		       inode->i_generation, ncopied, nmoved, ncopied + nmoved);
	while (ind > chain) {
		ind--;
		brelse(ind->bh);
	}
}
#endif
