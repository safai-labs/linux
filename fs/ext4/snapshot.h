/*
 * linux/fs/ext4/snapshot.h
 *
 * Written by Amir Goldstein <amir73il@users.sf.net>, 2008
 *
 * Copyright (C) 2008-2011 CTERA Networks
 *
 * This file is part of the Linux kernel and is made available under
 * the terms of the GNU General Public License, version 2, or at your
 * option, any later version, incorporated herein by reference.
 *
 * Ext4 snapshot extensions.
 */

#ifndef _LINUX_EXT4_SNAPSHOT_H
#define _LINUX_EXT4_SNAPSHOT_H

#include <linux/version.h>
#include <linux/delay.h>
#include "ext4.h"
#include "snapshot_debug.h"


/*
 * use signed 64bit for snapshot image addresses
 * negative addresses are used to reference snapshot meta blocks
 */
#define ext4_snapblk_t long long

/*
 * We assert that file system block size == page size (on mount time)
 * and that the first file system block is block 0 (on snapshot create).
 * Snapshot inode direct blocks are reserved for snapshot meta blocks.
 * Snapshot inode single indirect blocks are not used.
 * Snapshot image starts at the first double indirect block, so all blocks in
 * Snapshot image block group blocks are mapped by a single DIND block:
 * 4k: 32k blocks_per_group = 32 IND (4k) blocks = 32 groups per DIND
 * 8k: 64k blocks_per_group = 32 IND (8k) blocks = 64 groups per DIND
 * 16k: 128k blocks_per_group = 32 IND (16k) blocks = 128 groups per DIND
 */
#define SNAPSHOT_BLOCK_SIZE		PAGE_SIZE
#define SNAPSHOT_BLOCK_SIZE_BITS	PAGE_SHIFT
#define	SNAPSHOT_ADDR_PER_BLOCK		(SNAPSHOT_BLOCK_SIZE / sizeof(__u32))
#define SNAPSHOT_ADDR_PER_BLOCK_BITS	(SNAPSHOT_BLOCK_SIZE_BITS - 2)
#define SNAPSHOT_DIR_BLOCKS		EXT4_NDIR_BLOCKS
#define SNAPSHOT_IND_BLOCKS		SNAPSHOT_ADDR_PER_BLOCK

#define SNAPSHOT_BLOCKS_PER_GROUP_BITS	(SNAPSHOT_BLOCK_SIZE_BITS + 3)
#define SNAPSHOT_BLOCKS_PER_GROUP				\
	(1<<SNAPSHOT_BLOCKS_PER_GROUP_BITS) /* 8*PAGE_SIZE */
#define SNAPSHOT_BLOCK_GROUP(block)				\
	((block)>>SNAPSHOT_BLOCKS_PER_GROUP_BITS)
#define SNAPSHOT_BLOCK_GROUP_OFFSET(block)			\
	((block)&(SNAPSHOT_BLOCKS_PER_GROUP-1))
#define SNAPSHOT_BLOCK_TUPLE(block)				\
	(ext4_fsblk_t)SNAPSHOT_BLOCK_GROUP_OFFSET(block),	\
	(ext4_fsblk_t)SNAPSHOT_BLOCK_GROUP(block)
#define SNAPSHOT_IND_PER_BLOCK_GROUP_BITS			\
	(SNAPSHOT_BLOCKS_PER_GROUP_BITS-SNAPSHOT_ADDR_PER_BLOCK_BITS)
#define SNAPSHOT_IND_PER_BLOCK_GROUP				\
	(1<<SNAPSHOT_IND_PER_BLOCK_GROUP_BITS) /* 32 */
#define SNAPSHOT_DIND_BLOCK_GROUPS_BITS				\
	(SNAPSHOT_ADDR_PER_BLOCK_BITS-SNAPSHOT_IND_PER_BLOCK_GROUP_BITS)
#define SNAPSHOT_DIND_BLOCK_GROUPS				\
	(1<<SNAPSHOT_DIND_BLOCK_GROUPS_BITS)

#define SNAPSHOT_BLOCK_OFFSET					\
	(SNAPSHOT_DIR_BLOCKS+SNAPSHOT_IND_BLOCKS)
#define SNAPSHOT_BLOCK(iblock)					\
	((ext4_snapblk_t)(iblock) - SNAPSHOT_BLOCK_OFFSET)
#define SNAPSHOT_IBLOCK(block)					\
	(ext4_fsblk_t)((block) + SNAPSHOT_BLOCK_OFFSET)



#ifdef CONFIG_EXT4_FS_SNAPSHOT
#define EXT4_SNAPSHOT_VERSION "ext4 snapshot v1.0.13-7 (1-Jun-2010)"

#define SNAPSHOT_BYTES_OFFSET					\
	(SNAPSHOT_BLOCK_OFFSET << SNAPSHOT_BLOCK_SIZE_BITS)
#define SNAPSHOT_ISIZE(size)					\
	((size) + SNAPSHOT_BYTES_OFFSET)
/* Snapshot block device size is recorded in i_disksize */
#define SNAPSHOT_SET_SIZE(inode, size)				\
	(EXT4_I(inode)->i_disksize = SNAPSHOT_ISIZE(size))
#define SNAPSHOT_SIZE(inode)					\
	(EXT4_I(inode)->i_disksize - SNAPSHOT_BYTES_OFFSET)
#define SNAPSHOT_SET_BLOCKS(inode, blocks)			\
	SNAPSHOT_SET_SIZE((inode),				\
			(loff_t)(blocks) << SNAPSHOT_BLOCK_SIZE_BITS)
#define SNAPSHOT_BLOCKS(inode)					\
	(ext4_fsblk_t)(SNAPSHOT_SIZE(inode) >> SNAPSHOT_BLOCK_SIZE_BITS)
/* Snapshot shrink/merge/clean progress is exported via i_size */
#define SNAPSHOT_PROGRESS(inode)				\
	(ext4_fsblk_t)((inode)->i_size >> SNAPSHOT_BLOCK_SIZE_BITS)
#define SNAPSHOT_SET_ENABLED(inode)				\
	i_size_write((inode), SNAPSHOT_SIZE(inode))
#define SNAPSHOT_SET_PROGRESS(inode, blocks)			\
	snapshot_size_extend((inode), (blocks))
/* Disabled/deleted snapshot i_size is 1 block, to allow read of super block */
#define SNAPSHOT_SET_DISABLED(inode)				\
	snapshot_size_truncate((inode), 1)
/* Removed snapshot i_size and i_disksize are 0, since all blocks were freed */
#define SNAPSHOT_SET_REMOVED(inode)				\
	do {							\
		EXT4_I(inode)->i_disksize = 0;			\
		snapshot_size_truncate((inode), 0);		\
	} while (0)

static inline void snapshot_size_extend(struct inode *inode,
			ext4_fsblk_t blocks)
{
#ifdef CONFIG_EXT4_DEBUG
	ext4_fsblk_t old_blocks = SNAPSHOT_PROGRESS(inode);
	ext4_fsblk_t max_blocks = SNAPSHOT_BLOCKS(inode);

	/* sleep total of tunable delay unit over 100% progress */
	snapshot_test_delay_progress(SNAPTEST_DELETE,
			old_blocks, blocks, max_blocks);
#endif
	i_size_write((inode), (loff_t)(blocks) << SNAPSHOT_BLOCK_SIZE_BITS);
}

static inline void snapshot_size_truncate(struct inode *inode,
			ext4_fsblk_t blocks)
{
	loff_t i_size = (loff_t)blocks << SNAPSHOT_BLOCK_SIZE_BITS;

	i_size_write(inode, i_size);
	truncate_inode_pages(&inode->i_data, i_size);
}

/* Is ext4 configured for snapshots support? */
static inline int EXT4_SNAPSHOTS(struct super_block *sb)
{
	return EXT4_HAS_RO_COMPAT_FEATURE(sb,
			EXT4_FEATURE_RO_COMPAT_HAS_SNAPSHOT);
}

#define ext4_snapshot_cow(handle, inode, block, bh, cow) 0

#define ext4_snapshot_move(handle, inode, block, pcount, move) (0)

/*
 * Block access functions
 */

/*
 * get_write_access() is called before writing to a metadata block
 * if @inode is not NULL, then this is an inode's indirect block
 * otherwise, this is a file system global metadata block
 *
 * Return values:
 * = 0 - block was COWed or doesn't need to be COWed
 * < 0 - error
 */
static inline int ext4_snapshot_get_write_access(handle_t *handle,
		struct inode *inode, struct buffer_head *bh)
{
	struct super_block *sb;

	sb = handle->h_transaction->t_journal->j_private;
	if (!EXT4_SNAPSHOTS(sb))
		return 0;

	return ext4_snapshot_cow(handle, inode, bh->b_blocknr, bh, 1);
}

/*
 * get_create_access() is called after allocating a new metadata block
 *
 * Return values:
 * = 0 - block was COWed or doesn't need to be COWed
 * < 0 - error
 */
static inline int ext4_snapshot_get_create_access(handle_t *handle,
		struct buffer_head *bh)
{
	struct super_block *sb;
	int err;

	sb = handle->h_transaction->t_journal->j_private;
	if (!EXT4_SNAPSHOTS(sb))
		return 0;

	/* Should block be COWed? */
	err = ext4_snapshot_cow(handle, NULL, bh->b_blocknr, bh, 0);
	/*
	 * A new block shouldn't need to be COWed if get_delete_access() was
	 * called for all deleted blocks.  However, it may need to be COWed
	 * if fsck was run and if it had freed some blocks without moving them
	 * to snapshot.  In the latter case, -EIO will be returned.
	 */
	if (err > 0)
		err = -EIO;
	return err;
}

/*
 * get_bitmap_access() is called before modifying a block bitmap.
 * this call initializes the COW bitmap for @group.
 *
 * Return values:
 * = 0 - COW bitmap is initialized
 * < 0 - error
 */
static inline int ext4_snapshot_get_bitmap_access(handle_t *handle,
		struct super_block *sb, ext4_group_t group,
		struct buffer_head *bh)
{
	if (!EXT4_SNAPSHOTS(sb))
		return 0;
	/*
	 * With flex_bg, block bitmap may reside in a different group than
	 * the group it describes, so we need to init both COW bitmaps:
	 * 1. init the COW bitmap for @group by testing
	 *    if the first block in the group should be COWed
	 */
	if (EXT4_HAS_INCOMPAT_FEATURE(sb, EXT4_FEATURE_INCOMPAT_FLEX_BG)) {
		int err = ext4_snapshot_cow(handle, NULL,
				ext4_group_first_block_no(sb, group),
				NULL, 0);
		if (err < 0)
			return err;
	}
	/* 2. COW the block bitmap itself, which may be in another group */
	return ext4_snapshot_cow(handle, NULL, bh->b_blocknr, bh, 1);
}

/*
 * get_move_access() - move block to snapshot
 * @handle:	JBD handle
 * @inode:	owner of @block
 * @block:	address of @block
 * @pcount      pointer to no. of blocks about to move or approve
 * @move:	if false, only test if blocks need to be moved
 *
 * Called from ext4_ind_map_blocks() before overwriting a data block, when the
 * buffer_move_on_write() flag is set.  Specifically, only data blocks of
 * regular files are moved. Directory blocks are COWed on get_write_access().
 * Snapshots and excluded files blocks are never moved-on-write.
 * If @move is true, then down_write(&i_data_sem) is held.
 *
 * Return values:
 * > 0 - @blocks a) were moved for @move = 1;
 *		 b) need to be moved for @move = 0.
 * = 0 - blocks don't need to be moved.
 * < 0 - error
 */
static inline int ext4_snapshot_get_move_access(handle_t *handle,
						struct inode *inode,
						ext4_fsblk_t block,
						int *pcount, int move)
{
	if (!EXT4_SNAPSHOTS(inode->i_sb))
		return 0;

	return ext4_snapshot_move(handle, inode, block, pcount, move);
}

/*
 * get_delete_access() - move blocks to snapshot or approve to free them
 * @handle:	JBD handle
 * @inode:	owner of blocks if known (or NULL otherwise)
 * @block:	address of start @block
 * @pcount:	pointer to no. of blocks about to move or approve
 *
 * Called from ext4_free_blocks() before deleting blocks with
 * i_data_sem held
 *
 * Return values:
 * > 0 - blocks were moved to snapshot and may not be freed
 * = 0 - blocks may be freed
 * < 0 - error
 */
static inline int ext4_snapshot_get_delete_access(handle_t *handle,
		struct inode *inode, ext4_fsblk_t block, int *pcount)
{
	struct super_block *sb;

	sb = handle->h_transaction->t_journal->j_private;
	if (!EXT4_SNAPSHOTS(sb))
		return 0;

	return ext4_snapshot_move(handle, inode, block, pcount, 1);
}


/* snapshot_ctl.c */


static inline int init_ext4_snapshot(void)
{
	return 0;
}

static inline void exit_ext4_snapshot(void)
{
}





#else /* CONFIG_EXT4_FS_SNAPSHOT */

/* Snapshot NOP macros */
#define EXT4_SNAPSHOTS(sb) (0)
#define SNAPMAP_ISCOW(cmd)	(0)
#define SNAPMAP_ISMOVE(cmd)     (0)
#define SNAPMAP_ISSYNC(cmd)	(0)
#define IS_COWING(handle)	(0)

#define ext4_snapshot_load(sb, es, ro) (0)
#define ext4_snapshot_destroy(sb)
#define init_ext4_snapshot() (0)
#define exit_ext4_snapshot()
#define ext4_snapshot_active(sbi) (0)
#define ext4_snapshot_file(inode) (0)
#define ext4_snapshot_should_move_data(inode) (0)
#define ext4_snapshot_test_excluded(handle, inode, block_to_free, count) (0)
#define ext4_snapshot_list(inode) (0)
#define ext4_snapshot_get_flags(ei, filp)
#define ext4_snapshot_set_flags(handle, inode, flags) (0)
#define ext4_snapshot_take(inode) (0)
#define ext4_snapshot_update(inode_i_sb, cleanup, zero) (0)
#define ext4_snapshot_has_active(sb) (NULL)
#define ext4_snapshot_get_bitmap_access(handle, sb, grp, bh) (0)
#define ext4_snapshot_get_write_access(handle, inode, bh) (0)
#define ext4_snapshot_get_create_access(handle, bh) (0)
#define ext4_snapshot_excluded(ac_inode) (0)
#define ext4_snapshot_get_delete_access(handle, inode, block, pcount) (0)

#define ext4_snapshot_get_move_access(handle, inode, block, pcount, move) (0)
#define ext4_snapshot_start_pending_cow(sbh)
#define ext4_snapshot_end_pending_cow(sbh)
#define ext4_snapshot_is_active(inode)		(0)
#define ext4_snapshot_mow_in_tid(inode)		(1)

#endif /* CONFIG_EXT4_FS_SNAPSHOT */
#endif	/* _LINUX_EXT4_SNAPSHOT_H */
