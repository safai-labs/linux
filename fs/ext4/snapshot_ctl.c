/*
 * linux/fs/ext4/snapshot_ctl.c
 *
 * Written by Amir Goldstein <amir73il@users.sf.net>, 2008
 *
 * Copyright (C) 2008-2011 CTERA Networks
 *
 * This file is part of the Linux kernel and is made available under
 * the terms of the GNU General Public License, version 2, or at your
 * option, any later version, incorporated herein by reference.
 *
 * Ext4 snapshots control functions.
 */

#include <linux/statfs.h>
#include "ext4_jbd2.h"
#include "snapshot.h"

/*
 * General snapshot locking semantics:
 *
 * The snapshot_mutex:
 * -------------------
 * The majority of the code in the snapshot_{ctl,debug}.c files is called from
 * very few entry points in the code:
 * 1. {init,exit}_ext4_fs() - calls {init,exit}_ext4_snapshot() under BGL.
 * 2. ext4_{fill,put}_super() - calls ext4_snapshot_{load,destroy}() under
 *    VFS sb_lock, while f/s is not accessible to users.
 * 3. ext4_ioctl() - only place that takes snapshot_mutex (after i_mutex)
 *    and only entry point to snapshot control functions below.
 *
 * From the rules above it follows that all fields accessed inside
 * snapshot_{ctl,debug}.c are protected by one of the following:
 * - snapshot_mutex during snapshot control operations.
 * - VFS sb_lock during f/s mount/umount time.
 * - Big kernel lock during module init time.
 * Needless to say, either of the above is sufficient.
 * So if a field is accessed only inside snapshot_*.c it should be safe.
 *
 * The transaction handle:
 * -----------------------
 * Snapshot COW code (in snapshot.c) is called from block access hooks during a
 * transaction (with a transaction handle). This guaranties safe read access to
 * s_active_snapshot, without taking snapshot_mutex, because the latter is only
 * changed under journal_lock_updates() (while no transaction handles exist).
 *
 * The transaction handle is a per task struct, so there is no need to protect
 * fields on that struct (i.e. h_cowing, h_cow_*).
 */

/*
 * ext4_snapshot_set_active - set the current active snapshot
 * First, if current active snapshot exists, it is deactivated.
 * Then, if @inode is not NULL, the active snapshot is set to @inode.
 *
 * Called from ext4_snapshot_take() and ext4_snapshot_update() under
 * journal_lock_updates() and snapshot_mutex.
 * Called from ext4_snapshot_{load,destroy}() under sb_lock.
 *
 * Returns 0 on success and <0 on error.
 */
static int ext4_snapshot_set_active(struct super_block *sb,
		struct inode *inode)
{
	struct inode *old = EXT4_SB(sb)->s_active_snapshot;
	struct ext4_inode_info *ei = EXT4_I(inode);

	if (old == inode)
		return 0;

	/* add new active snapshot reference */
	if (inode && !igrab(inode))
		return -EIO;

	/* point of no return - replace old with new snapshot */
	if (old) {
		ext4_clear_inode_snapstate(old, EXT4_SNAPSTATE_ACTIVE);
		snapshot_debug(1, "snapshot (%u) deactivated\n",
			       old->i_generation);
		/* remove old active snapshot reference */
		iput(old);
	}
	if (inode) {
		/*
		 * Set up the jbd2_inode - we are about to file_inode soon...
		 */
		if (!ei->jinode) {
			struct jbd2_inode *jinode;
			jinode = jbd2_alloc_inode(GFP_KERNEL);

			spin_lock(&inode->i_lock);
			if (!ei->jinode) {
				if (!jinode) {
					spin_unlock(&inode->i_lock);
					return -ENOMEM;
				}
				ei->jinode = jinode;
				jbd2_journal_init_jbd_inode(ei->jinode, inode);
				jinode = NULL;
			}
			spin_unlock(&inode->i_lock);
			if (unlikely(jinode != NULL))
				jbd2_free_inode(jinode);
		}
		/* ACTIVE implies LIST */
		ext4_set_inode_snapstate(inode, EXT4_SNAPSTATE_LIST);
		ext4_set_inode_snapstate(inode, EXT4_SNAPSTATE_ACTIVE);
		snapshot_debug(1, "snapshot (%u) activated\n",
			       inode->i_generation);
	}
	EXT4_SB(sb)->s_active_snapshot = inode;

	return 0;
}
/*
 * ext4_snapshot_reset_bitmap_cache():
 *
 * Resets the COW/exclude bitmap cache for all block groups.
 *
 * Called from snapshot_take() under journal_lock_updates().
 */
static void ext4_snapshot_reset_bitmap_cache(struct super_block *sb)
{
	struct ext4_group_info *grp;
	int i;

	for (i = 0; i < EXT4_SB(sb)->s_groups_count; i++) {
		grp = ext4_get_group_info(sb, i);
		grp->bg_cow_bitmap = 0;
		cond_resched();
	}
}

/*
 * Snapshot control functions
 *
 * Snapshot files are controlled by changing snapshot flags with chattr and
 * moving the snapshot file through the stages of its life cycle:
 *
 * 1. Creating a snapshot file
 * The snapfile flag is changed for directories only (chattr +x), so
 * snapshot files must be created inside a snapshots directory.
 * They inherit the flag at birth and they die with it.
 * This helps to avoid various race conditions when changing
 * regular files to snapshots and back.
 * Snapshot files are assigned with read-only address space operations, so
 * they are not writable for users.
 *
 * 2. Taking a snapshot
 * An empty snapshot file becomes the active snapshot after it is added to the
 * head on the snapshots list by setting its snapshot list flag (chattr -X +S).
 * snapshot_create() verifies that the file is empty and pre-allocates some
 * blocks during the ioctl transaction.  snapshot_take() locks journal updates
 * and copies some file system block to the pre-allocated blocks and then adds
 * the snapshot file to the on-disk list and sets it as the active snapshot.
 *
 * 3. Mounting a snapshot
 * A snapshot on the list can be enabled for user read access by setting the
 * enabled flag (chattr -X +n) and disabled by clearing the enabled flag.
 * An enabled snapshot can be mounted via a loop device and mounted as a
 * read-only ext2 filesystem.
 *
 * 4. Deleting a snapshot
 * A non-mounted and disabled snapshot may be marked for removal from the
 * snapshots list by requesting to clear its snapshot list flag (chattr -X -S).
 * The process of removing a snapshot from the list varies according to the
 * dependencies between the snapshot and older snapshots on the list:
 * - if all older snapshots are deleted, the snapshot is removed from the list.
 * - if some older snapshots are enabled, snapshot_shrink() is called to free
 *   unused blocks, but the snapshot remains on the list.
 * - if all older snapshots are disabled, snapshot_merge() is called to move
 *   used blocks to an older snapshot and the snapshot is removed from the list.
 *
 * 5. Unlinking a snapshot file
 * When a snapshot file is no longer (or never was) on the snapshots list, it
 * may be unlinked.  Snapshots on the list are protected from user unlink and
 * truncate operations.
 *
 * 6. Discarding all snapshots
 * An irregular way to abruptly end the lives of all snapshots on the list is by
 * detaching the snapshot list head using the command: tune2fs -O ^has_snapshot.
 * This action is applicable on an un-mounted ext4 filesystem.  After mounting
 * the filesystem, the discarded snapshot files will not be loaded, they will
 * not have the snapshot list flag and therefore, may be unlinked.
 */
static int ext4_snapshot_enable(struct inode *inode);
static int ext4_snapshot_disable(struct inode *inode);
static int ext4_snapshot_create(struct inode *inode);
static int ext4_snapshot_delete(struct inode *inode);

/*
 * ext4_snapshot_get_flags() check snapshot state
 * Called from ext4_ioctl() under i_mutex
 */
void ext4_snapshot_get_flags(struct inode *inode, struct file *filp)
{
	unsigned int open_count = filp->f_path.dentry->d_count;

	/*
	 * 1 count for ioctl (lsattr)
	 * greater count means the snapshot is open by user (mounted?)
	 * We rely on d_count because snapshot shouldn't have hard links.
	 */
	if (ext4_snapshot_list(inode) && open_count > 1)
		ext4_set_inode_snapstate(inode, EXT4_SNAPSTATE_OPEN);
	else
		ext4_clear_inode_snapstate(inode, EXT4_SNAPSTATE_OPEN);
	/* copy persistent flags to dynamic state flags */
	if (ext4_test_inode_flag(inode, EXT4_INODE_SNAPFILE_DELETED))
		ext4_set_inode_snapstate(inode, EXT4_SNAPSTATE_DELETED);
	else
		ext4_clear_inode_snapstate(inode, EXT4_SNAPSTATE_DELETED);
	if (ext4_test_inode_flag(inode, EXT4_INODE_SNAPFILE_SHRUNK))
		ext4_set_inode_snapstate(inode, EXT4_SNAPSTATE_SHRUNK);
	else
		ext4_clear_inode_snapstate(inode, EXT4_SNAPSTATE_SHRUNK);
}

/*
 * ext4_snapshot_set_flags() monitors snapshot state changes
 * Called from ext4_ioctl() under i_mutex and snapshot_mutex
 */
int ext4_snapshot_set_flags(handle_t *handle, struct inode *inode,
			     unsigned int flags)
{
	unsigned int oldflags = ext4_get_snapstate_flags(inode);
	int err = 0;

	if ((flags ^ oldflags) & 1UL<<EXT4_SNAPSTATE_ENABLED) {
		/* enabled/disabled the snapshot during transaction */
		if (flags & 1UL<<EXT4_SNAPSTATE_ENABLED)
			err = ext4_snapshot_enable(inode);
		else
			err = ext4_snapshot_disable(inode);
	}
	if (err)
		goto out;

	if ((flags ^ oldflags) & 1UL<<EXT4_SNAPSTATE_LIST) {
		/* add/delete to snapshots list during transaction */
		if (flags & 1UL<<EXT4_SNAPSTATE_LIST)
			err = ext4_snapshot_create(inode);
		else
			err = ext4_snapshot_delete(inode);
	}
	if (err)
		goto out;

out:
	/*
	 * retake reserve inode write from ext4_ioctl() and mark inode
	 * dirty
	 */
	if (!err)
		err = ext4_mark_inode_dirty(handle, inode);
	return err;
}

/*
 * If we have fewer than nblocks credits,
 * extend transaction by at most EXT4_MAX_TRANS_DATA.
 * If that fails, restart the transaction &
 * regain write access for the inode block.
 */
int __extend_or_restart_transaction(const char *where,
		handle_t *handle, struct inode *inode, int nblocks)
{
	int err;

	if (ext4_handle_has_enough_credits(handle, nblocks))
		return 0;

	if (nblocks < EXT4_MAX_TRANS_DATA)
		nblocks = EXT4_MAX_TRANS_DATA;

	err = __ext4_journal_extend(where, handle, nblocks);
	if (err < 0)
		return err;
	if (err) {
		if (inode) {
			/* lazy way to do mark_iloc_dirty() */
			err = ext4_mark_inode_dirty(handle, inode);
			if (err)
				return err;
		}
		err = __ext4_journal_restart(where, handle, nblocks);
		if (err)
			return err;
		if (inode)
			/* lazy way to do reserve_inode_write() */
			err = ext4_mark_inode_dirty(handle, inode);
	}

	return err;
}

#define extend_or_restart_transaction(handle, nblocks)			\
	__extend_or_restart_transaction(__func__, (handle), NULL, (nblocks))
#define extend_or_restart_transaction_inode(handle, inode, nblocks)	\
	__extend_or_restart_transaction(__func__, (handle), (inode), (nblocks))


static ext4_fsblk_t ext4_get_inode_block(struct super_block *sb,
					 unsigned long ino,
					 struct ext4_iloc *iloc)
{
	ext4_fsblk_t block;
	struct ext4_group_desc *desc;
	int inodes_per_block, inode_offset;

	iloc->bh = NULL;
	iloc->offset = 0;
	iloc->block_group = 0;

	if (!ext4_valid_inum(sb, ino))
		return 0;

	iloc->block_group = (ino - 1) / EXT4_INODES_PER_GROUP(sb);
	desc = ext4_get_group_desc(sb, iloc->block_group, NULL);
	if (!desc)
		return 0;

	/*
	 * Figure out the offset within the block group inode table
	 */
	inodes_per_block = (EXT4_BLOCK_SIZE(sb) / EXT4_INODE_SIZE(sb));
	inode_offset = ((ino - 1) %
			EXT4_INODES_PER_GROUP(sb));
	block = ext4_inode_table(sb, desc) + (inode_offset / inodes_per_block);
	iloc->offset = (inode_offset % inodes_per_block) * EXT4_INODE_SIZE(sb);
	return block;
}

/*
 * ext4_snapshot_create() initializes a snapshot file
 * and adds it to the list of snapshots
 * Called under i_mutex and snapshot_mutex
 */
static int ext4_snapshot_create(struct inode *inode)
{
	handle_t *handle;
	struct super_block *sb = inode->i_sb;
	struct ext4_sb_info *sbi = EXT4_SB(sb);
	struct inode *active_snapshot = ext4_snapshot_has_active(sb);
	struct ext4_inode_info *ei = EXT4_I(inode);
	int i, err, ret;
	ext4_fsblk_t snapshot_blocks = ext4_blocks_count(sbi->s_es);
	if (active_snapshot) {
		snapshot_debug(1, "failed to add snapshot because active "
			       "snapshot (%u) has to be deleted first\n",
			       active_snapshot->i_generation);
		return -EINVAL;
	}

	/* prevent take of unlinked snapshot file */
	if (!inode->i_nlink) {
		snapshot_debug(1, "failed to create snapshot file (ino=%lu) "
				"because it has 0 nlink count\n",
				inode->i_ino);
		return -EINVAL;
	}

	/* prevent recycling of old snapshot files */
	if (ext4_test_inode_flag(inode, EXT4_INODE_SNAPFILE_DELETED)) {
		snapshot_debug(1, "deleted snapshot file (ino=%lu) cannot "
				"be reused - it may be unlinked\n",
				inode->i_ino);
		return -EINVAL;
	}

	/* verify that no inode blocks are allocated */
	for (i = 0; i < EXT4_N_BLOCKS; i++) {
		if (ei->i_data[i])
			break;
	}
	/* Don't need i_size_read because we hold i_mutex */
	if (i != EXT4_N_BLOCKS ||
		inode->i_size > 0 || ei->i_disksize > 0) {
		snapshot_debug(1, "failed to create snapshot file (ino=%lu) "
				"because it is not empty (i_data[%d]=%u, "
				"i_size=%lld, i_disksize=%lld)\n",
				inode->i_ino, i, ei->i_data[i],
				inode->i_size, ei->i_disksize);
		return -EINVAL;
	}

	/*
	 * Take a reference to the small transaction that started in
	 * ext4_ioctl() We will extend or restart this transaction as we go
	 * along.  journal_start(n > 1) would not have increase the buffer
	 * credits.
	 */
	handle = ext4_journal_start(inode, 1);

	err = extend_or_restart_transaction_inode(handle, inode, 2);
	if (err)
		goto out_handle;

	/* record the new snapshot ID in the snapshot inode generation field */
	inode->i_generation = le32_to_cpu(sbi->s_es->s_snapshot_id) + 1;
	if (inode->i_generation == 0)
		/* 0 is not a valid snapshot id */
		inode->i_generation = 1;

	/* record the file system size in the snapshot inode disksize field */
	SNAPSHOT_SET_BLOCKS(inode, snapshot_blocks);

	lock_super(sb);
	err = ext4_journal_get_write_access(handle, sbi->s_sbh);
	sbi->s_es->s_snapshot_list = cpu_to_le32(inode->i_ino);
	if (!err)
		err = ext4_handle_dirty_metadata(handle, NULL, sbi->s_sbh);
	unlock_super(sb);
	if (err)
		goto out_handle;

	err = ext4_mark_inode_dirty(handle, inode);
	if (err)
		goto out_handle;

	snapshot_debug(1, "snapshot (%u) created\n", inode->i_generation);
	err = 0;
out_handle:
	ret = ext4_journal_stop(handle);
	if (!err)
		err = ret;
	return err;
}


/*
 * ext4_snapshot_take() makes a new snapshot file
 * into the active snapshot
 *
 * this function calls journal_lock_updates()
 * and should not be called during a journal transaction
 * Called from ext4_ioctl() under i_mutex and snapshot_mutex
 */
int ext4_snapshot_take(struct inode *inode)
{
	struct super_block *sb = inode->i_sb;
	struct ext4_sb_info *sbi = EXT4_SB(sb);
	struct ext4_super_block *es = NULL;
	struct buffer_head *es_bh = NULL;
	struct buffer_head *sbh = NULL;
	int err = -EIO;

	if (!sbi->s_sbh)
		goto out_err;
	else if (sbi->s_sbh->b_blocknr != 0) {
		snapshot_debug(1, "warning: unexpected super block at block "
			"(%lld:%d)!\n", (long long)sbi->s_sbh->b_blocknr,
			(int)((char *)sbi->s_es - (char *)sbi->s_sbh->b_data));
	} else if (sbi->s_es->s_magic != cpu_to_le16(EXT4_SUPER_MAGIC)) {
		snapshot_debug(1, "warning: super block of snapshot (%u) is "
			       "broken!\n", inode->i_generation);
	} else
		es_bh = ext4_getblk(NULL, inode, SNAPSHOT_IBLOCK(0),
				   SNAPMAP_READ, &err);

	if (!es_bh || es_bh->b_blocknr == 0) {
		snapshot_debug(1, "warning: super block of snapshot (%u) not "
			       "allocated\n", inode->i_generation);
		goto out_err;
	} else {
		snapshot_debug(4, "super block of snapshot (%u) mapped to "
			       "block (%lld)\n", inode->i_generation,
			       (long long)es_bh->b_blocknr);
		es = (struct ext4_super_block *)(es_bh->b_data +
						  ((char *)sbi->s_es -
						   sbi->s_sbh->b_data));
	}

	err = -EIO;

	/*
	 * flush journal to disk and clear the RECOVER flag
	 * before taking the snapshot
	 */
	freeze_super(sb);
	lock_super(sb);

#ifdef CONFIG_EXT4_DEBUG
	if (snapshot_enable_test[SNAPTEST_TAKE]) {
		snapshot_debug(1, "taking snapshot (%u) ...\n",
				inode->i_generation);
		/* sleep 1 tunable delay unit */
		snapshot_test_delay(SNAPTEST_TAKE);
	}
#endif


	/* reset i_size and invalidate page cache */
	SNAPSHOT_SET_DISABLED(inode);
	/* reset COW bitmap cache */
	ext4_snapshot_reset_bitmap_cache(sb);
	/* set as in-memory active snapshot */
	err = ext4_snapshot_set_active(sb, inode);
	if (err)
		goto out_unlockfs;

	/* set as on-disk active snapshot */

	sbi->s_es->s_snapshot_id =
		cpu_to_le32(le32_to_cpu(sbi->s_es->s_snapshot_id) + 1);
	if (sbi->s_es->s_snapshot_id == 0)
		/* 0 is not a valid snapshot id */
		sbi->s_es->s_snapshot_id = cpu_to_le32(1);
	sbi->s_es->s_snapshot_inum = cpu_to_le32(inode->i_ino);
	ext4_snapshot_set_tid(sb);

	err = 0;
out_unlockfs:
	unlock_super(sb);
	thaw_super(sb);

	if (err)
		goto out_err;

	snapshot_debug(1, "snapshot (%u) has been taken\n",
			inode->i_generation);

out_err:
	brelse(es_bh);
	brelse(sbh);
	return err;
}

/*
 * ext4_snapshot_enable() enables snapshot mount
 * sets the in-use flag and the active snapshot
 * Called under i_mutex and snapshot_mutex
 */
static int ext4_snapshot_enable(struct inode *inode)
{
	if (!ext4_snapshot_list(inode)) {
		snapshot_debug(1, "ext4_snapshot_enable() called with "
			       "snapshot file (ino=%lu) not on list\n",
			       inode->i_ino);
		return -EINVAL;
	}

	if (ext4_test_inode_flag(inode, EXT4_INODE_SNAPFILE_DELETED)) {
		snapshot_debug(1, "enable of deleted snapshot (%u) "
				"is not permitted\n",
				inode->i_generation);
		return -EPERM;
	}

	/*
	 * set i_size to block device size to enable loop device mount
	 */
	SNAPSHOT_SET_ENABLED(inode);
	ext4_set_inode_snapstate(inode, EXT4_SNAPSTATE_ENABLED);

	/* Don't need i_size_read because we hold i_mutex */
	snapshot_debug(4, "setting snapshot (%u) i_size to (%lld)\n",
			inode->i_generation, inode->i_size);
	snapshot_debug(1, "snapshot (%u) enabled\n", inode->i_generation);
	return 0;
}

/*
 * ext4_snapshot_disable() disables snapshot mount
 * Called under i_mutex and snapshot_mutex
 */
static int ext4_snapshot_disable(struct inode *inode)
{
	if (!ext4_snapshot_list(inode)) {
		snapshot_debug(1, "ext4_snapshot_disable() called with "
			       "snapshot file (ino=%lu) not on list\n",
			       inode->i_ino);
		return -EINVAL;
	}

	if (ext4_test_inode_snapstate(inode, EXT4_SNAPSTATE_OPEN)) {
		snapshot_debug(1, "disable of mounted snapshot (%u) "
			       "is not permitted\n",
			       inode->i_generation);
		return -EPERM;
	}

	/* reset i_size and invalidate page cache */
	SNAPSHOT_SET_DISABLED(inode);
	ext4_clear_inode_snapstate(inode, EXT4_SNAPSTATE_ENABLED);

	/* Don't need i_size_read because we hold i_mutex */
	snapshot_debug(4, "setting snapshot (%u) i_size to (%lld)\n",
		       inode->i_generation, inode->i_size);
	snapshot_debug(1, "snapshot (%u) disabled\n", inode->i_generation);
	return 0;
}

/*
 * ext4_snapshot_delete() marks snapshot for deletion
 * Called under i_mutex and snapshot_mutex
 */
static int ext4_snapshot_delete(struct inode *inode)
{
	if (!ext4_snapshot_list(inode)) {
		snapshot_debug(1, "ext4_snapshot_delete() called with "
			       "snapshot file (ino=%lu) not on list\n",
			       inode->i_ino);
		return -EINVAL;
	}

	if (ext4_test_inode_snapstate(inode, EXT4_SNAPSTATE_ENABLED)) {
		snapshot_debug(1, "delete of enabled snapshot (%u) "
			       "is not permitted\n",
			       inode->i_generation);
		return -EPERM;
	}

	/* mark deleted for later cleanup to finish the job */
	ext4_set_inode_flag(inode, EXT4_INODE_SNAPFILE_DELETED);
	snapshot_debug(1, "snapshot (%u) marked for deletion\n",
		       inode->i_generation);
	return 0;
}

/*
 * ext4_snapshot_remove - removes a snapshot from the list
 * @inode: snapshot inode
 *
 * Removed the snapshot inode from in-memory and on-disk snapshots list of
 * and truncates the snapshot inode.
 * Called from ext4_snapshot_update/cleanup/merge() under snapshot_mutex.
 * Returns 0 on success and <0 on error.
 */
static int ext4_snapshot_remove(struct inode *inode)
{
	handle_t *handle;
	struct ext4_sb_info *sbi;
	int err = 0, ret;

	/* elevate ref count until final cleanup */
	if (!igrab(inode))
		return -EIO;

	if (ext4_test_inode_snapstate(inode, EXT4_SNAPSTATE_ACTIVE) ||
		ext4_test_inode_snapstate(inode, EXT4_SNAPSTATE_ENABLED) ||
		ext4_test_inode_snapstate(inode, EXT4_SNAPSTATE_INUSE)) {
		snapshot_debug(1, "ext4_snapshot_remove() called with active/"
			       "enabled/in-use snapshot file (ino=%lu)\n",
			       inode->i_ino);
		err = -EINVAL;
		goto out_err;
	}

	/* start large truncate transaction that will be extended/restarted */
	handle = ext4_journal_start(inode, EXT4_MAX_TRANS_DATA);
	if (IS_ERR(handle)) {
		err = PTR_ERR(handle);
		goto out_err;
	}
	sbi = EXT4_SB(inode->i_sb);


	err = extend_or_restart_transaction_inode(handle, inode, 2);
	if (err)
		goto out_handle;

	lock_super(inode->i_sb);
	err = ext4_journal_get_write_access(handle, sbi->s_sbh);
	sbi->s_es->s_snapshot_list = 0;
	if (!err)
		err = ext4_handle_dirty_metadata(handle, NULL, sbi->s_sbh);
	unlock_super(inode->i_sb);
	if (err)
		goto out_handle;
	/*
	 * At this point, this snapshot is empty and not on the snapshots list.
	 * As long as it was on the list it had to have the LIST flag to prevent
	 * truncate/unlink.  Now that it is removed from the list, the LIST flag
	 * and other snapshot status flags should be cleared.  It will still
	 * have the SNAPFILE and SNAPFILE_DELETED persistent flags to indicate
	 * this is a deleted snapshot that should not be recycled.
	 */
	ext4_clear_inode_snapstate(inode, EXT4_SNAPSTATE_LIST);
	ext4_clear_inode_snapstate(inode, EXT4_SNAPSTATE_ENABLED);
	ext4_clear_inode_snapstate(inode, EXT4_SNAPSTATE_ACTIVE);
	ext4_clear_inode_snapstate(inode, EXT4_SNAPSTATE_INUSE);

out_handle:
	ret = ext4_journal_stop(handle);
	if (!err)
		err = ret;
	if (err)
		goto out_err;

	snapshot_debug(1, "snapshot (%u) deleted\n", inode->i_generation);

	err = 0;
out_err:
	/* drop final ref count - taken on entry to this function */
	iput(inode);
	if (err) {
		snapshot_debug(1, "failed to delete snapshot (%u)\n",
				inode->i_generation);
	}
	return err;
}

/*
 * Snapshot constructor/destructor
 */
/*
 * ext4_snapshot_load - load the on-disk snapshot list to memory.
 * Start with last (or active) snapshot and continue to older snapshots.
 * If snapshot load fails before active snapshot, force read-only mount.
 * If snapshot load fails after active snapshot, allow read-write mount.
 * Called from ext4_fill_super() under sb_lock during mount time.
 *
 * Return values:
 * = 0 - on-disk snapshot list is empty or active snapshot loaded
 * < 0 - error loading active snapshot
 */
int ext4_snapshot_load(struct super_block *sb, struct ext4_super_block *es,
		int read_only)
{
	__u32 active_ino = le32_to_cpu(es->s_snapshot_inum);
	__u32 load_ino = le32_to_cpu(es->s_snapshot_list);
	int err = 0, num = 0, snapshot_id = 0;
	int has_active = 0;


	if (!load_ino && active_ino) {
		/* snapshots list is empty and active snapshot exists */
		if (!read_only)
			/* reset list head to active snapshot */
			es->s_snapshot_list = es->s_snapshot_inum;
		/* try to load active snapshot */
		load_ino = le32_to_cpu(es->s_snapshot_inum);
	}

	while (load_ino) {
		struct inode *inode;

		inode = ext4_orphan_get(sb, load_ino);
		if (IS_ERR(inode)) {
			err = PTR_ERR(inode);
		} else if (!ext4_snapshot_file(inode)) {
			iput(inode);
			err = -EIO;
		}

		if (err && num == 0 && load_ino != active_ino) {
			/* failed to load last non-active snapshot */
			if (!read_only)
				/* reset list head to active snapshot */
				es->s_snapshot_list = es->s_snapshot_inum;
			snapshot_debug(1, "warning: failed to load "
					"last snapshot (%u) - trying to load "
					"active snapshot (%u).\n",
					load_ino, active_ino);
			/* try to load active snapshot */
			load_ino = active_ino;
			err = 0;
			continue;
		}

		if (err)
			break;

		snapshot_id = inode->i_generation;
		snapshot_debug(1, "snapshot (%d) loaded\n",
			       snapshot_id);
		num++;

		if (!has_active && load_ino == active_ino) {
			/* active snapshot was loaded */
			err = ext4_snapshot_set_active(sb, inode);
			if (err)
				break;
			has_active = 1;
		}

		iput(inode);
		break;
	}

	if (err) {
		/* failed to load active snapshot */
		snapshot_debug(1, "warning: failed to load "
				"snapshot (ino=%u) - "
				"forcing read-only mount!\n",
				load_ino);
		/* force read-only mount */
		return read_only ? 0 : err;
	}

	if (num > 0) {
		err = ext4_snapshot_update(sb, 0, read_only);
		snapshot_debug(1, "%d snapshots loaded\n", num);
	}
	return err;
}

/*
 * ext4_snapshot_destroy() releases the in-memory snapshot list
 * Called from ext4_put_super() under sb_lock during umount time.
 * This function cannot fail.
 */
void ext4_snapshot_destroy(struct super_block *sb)
{
	/* deactivate in-memory active snapshot - cannot fail */
	(void) ext4_snapshot_set_active(sb, NULL);
}

/*
 * ext4_snapshot_update - iterate snapshot list and update snapshots status.
 * @sb: handle to file system super block.
 * @cleanup: if true, shrink/merge/cleanup all snapshots marked for deletion.
 * @read_only: if true, don't remove snapshot after failed take.
 *
 * Called from ext4_ioctl() under snapshot_mutex.
 * Called from snapshot_load() under sb_lock with @cleanup=0.
 * Returns 0 on success and <0 on error.
 */
int ext4_snapshot_update(struct super_block *sb, int cleanup, int read_only)
{
	struct inode *active_snapshot = ext4_snapshot_has_active(sb);
	struct inode *used_by = NULL; /* last non-deleted snapshot found */
	int deleted;
	int err = 0;

	BUG_ON(read_only && cleanup);
	if (active_snapshot) {
		/* ACTIVE implies LIST */
		ext4_set_inode_snapstate(active_snapshot,
					EXT4_SNAPSTATE_LIST);
		ext4_set_inode_snapstate(active_snapshot,
					EXT4_SNAPSTATE_ACTIVE);
	}


	if (!active_snapshot || !cleanup || used_by)
		return 0;

	/* if all snapshots are deleted - deactivate active snapshot */
	deleted = ext4_test_inode_flag(active_snapshot,
				       EXT4_INODE_SNAPFILE_DELETED);
	if (deleted && igrab(active_snapshot)) {
		/* lock journal updates before deactivating snapshot */
		freeze_super(sb);
		lock_super(sb);
		/* deactivate in-memory active snapshot - cannot fail */
		(void) ext4_snapshot_set_active(sb, NULL);
		/* clear on-disk active snapshot */
		EXT4_SB(sb)->s_es->s_snapshot_inum = 0;
		unlock_super(sb);
		thaw_super(sb);
		/* remove unused deleted active snapshot */
		err = ext4_snapshot_remove(active_snapshot);
		/* drop the refcount to 0 */
		iput(active_snapshot);
	}
	return err;
}
