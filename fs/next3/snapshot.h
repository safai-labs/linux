/*
 * linux/fs/next3/snapshot.h
 *
 * Written by Amir Goldstein <amir@ctera.com>, 2008
 *
 * Copyright (C) 2008-2010 CTERA Networks
 *
 * This file is part of the Linux kernel and is made available under
 * the terms of the GNU General Public License, version 2, or at your
 * option, any later version, incorporated herein by reference.
 *
 * Next3 snapshot extensions.
 */

#ifndef _LINUX_NEXT3_SNAPSHOT_H
#define _LINUX_NEXT3_SNAPSHOT_H

#include "next3_jbd.h"
#include "snapshot_debug.h"


#define NEXT3_SNAPSHOT_VERSION "next3 snapshot v1.0.0-rc7"

/*
 * use signed 64bit for snapshot image addresses
 * negative addresses are used to reference snapshot meta blocks
 */
#define next3_snapblk_t long long

/*
 * We assert that snapshot must use a file system with block size == page
 * size (4K) and that the first file system block is block 0.
 * Snapshot inode direct blocks are reserved for snapshot meta blocks.
 * Snapshot inode single indirect blocks are not used.
 * Snapshot image starts at the first double indirect block.
 * This way, a snapshot image block group can be mapped with 1 double
 * indirect block + 32 indirect blocks.  To mount the snapshot image, the
 * loop device should be configured with:
 * '-o <SNAPSHOT_BYTES_OFFSET>' (i.e. (12+1k)*4k = 4243456)
 */
#define SNAPSHOT_BLOCK_SIZE		PAGE_SIZE
#define SNAPSHOT_BLOCK_SIZE_BITS	PAGE_SHIFT
#define	SNAPSHOT_ADDR_PER_BLOCK		(SNAPSHOT_BLOCK_SIZE / sizeof(__u32))
#define SNAPSHOT_ADDR_PER_BLOCK_BITS	(SNAPSHOT_BLOCK_SIZE_BITS - 2)
#define SNAPSHOT_DIR_BLOCKS		NEXT3_NDIR_BLOCKS
#define SNAPSHOT_IND_BLOCKS		SNAPSHOT_ADDR_PER_BLOCK

#define SNAPSHOT_BLOCKS_PER_GROUP_BITS	15
#define SNAPSHOT_BLOCKS_PER_GROUP					\
	(1<<SNAPSHOT_BLOCKS_PER_GROUP_BITS) /* 32K */
#define SNAPSHOT_BLOCK_GROUP(block)		\
	((block)>>SNAPSHOT_BLOCKS_PER_GROUP_BITS)
#define SNAPSHOT_BLOCK_GROUP_OFFSET(block)	\
	((block)&(SNAPSHOT_BLOCKS_PER_GROUP-1))
#define SNAPSHOT_IND_PER_BLOCK_GROUP_BITS				\
	(SNAPSHOT_BLOCKS_PER_GROUP_BITS-SNAPSHOT_ADDR_PER_BLOCK_BITS)
#define SNAPSHOT_IND_PER_BLOCK_GROUP			\
	(1<<SNAPSHOT_IND_PER_BLOCK_GROUP_BITS) /* 32 */
#define SNAPSHOT_DIND_BLOCK_GROUPS_BITS					\
	(SNAPSHOT_ADDR_PER_BLOCK_BITS-SNAPSHOT_IND_PER_BLOCK_GROUP_BITS)
#define SNAPSHOT_DIND_BLOCK_GROUPS			\
	(1<<SNAPSHOT_DIND_BLOCK_GROUPS_BITS) /* 32 */

#define SNAPSHOT_BLOCK_OFFSET				\
	(SNAPSHOT_DIR_BLOCKS+SNAPSHOT_IND_BLOCKS)
#define SNAPSHOT_BLOCK(iblock)					\
	((next3_snapblk_t)(iblock) - SNAPSHOT_BLOCK_OFFSET)
#define SNAPSHOT_IBLOCK(block)						\
	(next3_fsblk_t)((block) + SNAPSHOT_BLOCK_OFFSET)
#define SNAPSHOT_META_BLOCK(inode, iblock)	\
	(NEXT3_I(inode)->i_data[iblock])

#define SNAPSHOT_BYTES_OFFSET					\
	(SNAPSHOT_BLOCK_OFFSET << SNAPSHOT_BLOCK_SIZE_BITS)
#define SNAPSHOT_ISIZE(size)			\
	((size) + SNAPSHOT_BYTES_OFFSET)

/*
 * snapshot meta blocks:
 */
#define SNAPSHOT_META_DIND	0 /* dind is pre-allocated here */
#define SNAPSHOT_META_TIND	1 /* tind is pre-allocated here */
#define SNAPSHOT_META_BLOCKS	2

#define SNAPSHOT_META_SIZE					\
	(SNAPSHOT_META_BLOCKS << SNAPSHOT_BLOCK_SIZE_BITS)
#define SNAPSHOT_SET_SIZE(inode, size)				\
	(NEXT3_I(inode)->i_disksize = SNAPSHOT_ISIZE(size))
#define SNAPSHOT_SIZE(inode)					\
	(NEXT3_I(inode)->i_disksize - SNAPSHOT_BYTES_OFFSET)
#define SNAPSHOT_BLOCKS(inode)						\
	(SNAPSHOT_SIZE(inode) >> SNAPSHOT_BLOCK_SIZE_BITS)
#define SNAPSHOT_SET_ENABLED(inode)				\
	i_size_write((inode), NEXT3_I(inode)->i_disksize)
#define SNAPSHOT_SET_DISABLED(inode)		\
	i_size_write((inode), 0)

/*
 * Block access functions
 */
#ifdef CONFIG_NEXT3_FS_SNAPSHOT_FILE
extern int next3_snapshot_get_inode_access(handle_t *handle,
					   struct inode *inode,
					   next3_fsblk_t iblock,
					   int count, int cmd,
					   struct inode **prev_snapshot);
#endif
#ifdef CONFIG_NEXT3_FS_SNAPSHOT_HOOKS
extern int next3_snapshot_get_undo_access(handle_t *handle,
					  struct buffer_head *bh);
extern int next3_snapshot_get_write_access(handle_t *handle,
					   struct inode *inode,
					   struct buffer_head *bh);
extern int next3_snapshot_get_create_access(handle_t *handle,
					    struct buffer_head *bh);
extern int next3_snapshot_get_move_access(handle_t *handle,
					  struct inode *inode,
					  next3_fsblk_t block, int move);
extern int next3_snapshot_get_delete_access(handle_t *handle,
					    struct inode *inode,
					    next3_fsblk_t block, int count);
#endif
#ifdef CONFIG_NEXT3_FS_SNAPSHOT_FILE_EXCLUDE
extern int next3_snapshot_get_clear_access(handle_t *handle,
					   struct inode *inode,
					   next3_fsblk_t block, int count);
#endif

#ifdef CONFIG_NEXT3_FS_SNAPSHOT_CTL
/*
 * Snapshot control functions
 */
extern void next3_snapshot_get_flags(struct next3_inode_info *ei,
				     struct file *filp);
extern int next3_snapshot_set_flags(handle_t *handle, struct inode *inode,
				    unsigned int flags);
extern int next3_snapshot_take(struct inode *inode);

/*
 * snapshot_map_blocks() command flags passed to get_blocks_handle() on its
 * @create argument.  All places in original code call get_blocks_handle()
 * with @create 0 or 1.  The behavior of the function remains the same for
 * these 2 values, while higher bits are used for mapping snapshot blocks.
 */
/* original meaning - only check if blocks are mapped */
#define SNAPMAP_READ	0
/* original meaning - allocate missing blocks and indirect blocks */
#define SNAPMAP_WRITE	0x1
/* creating COWed block - handle COW race conditions */
#define SNAPMAP_COW	0x2
/* moving blocks to snapshot - allocate only indirect blocks */
#define SNAPMAP_MOVE	0x4
/* bypass journal and sync allocated indirect blocks directly to disk */
#define SNAPMAP_SYNC	0x8
/* creating COW bitmap - handle COW races and bypass journal */
#define SNAPMAP_BITMAP	(SNAPMAP_COW|SNAPMAP_SYNC)

/* original @create flag test - only check map or create map? */
#define SNAPMAP_ISREAD(cmd)	((cmd) == SNAPMAP_READ)
#define SNAPMAP_ISWRITE(cmd)	((cmd) == SNAPMAP_WRITE)
#define SNAPMAP_ISCREATE(cmd)	((cmd) != SNAPMAP_READ)
/* test special cases when mapping snapshot blocks */
#define SNAPMAP_ISSPECIAL(cmd)	((cmd) & ~SNAPMAP_WRITE)
#define SNAPMAP_ISCOW(cmd)	((cmd) & SNAPMAP_COW)
#define SNAPMAP_ISMOVE(cmd)	((cmd) & SNAPMAP_MOVE)
#define SNAPMAP_ISSYNC(cmd)	((cmd) & SNAPMAP_SYNC)

/* helper functions for next3_snapshot_create() */
extern int next3_snapshot_map_blocks(handle_t *handle, struct inode *inode,
				     next3_snapblk_t block,
				     unsigned long maxblocks,
				     next3_fsblk_t *mapped, int cmd);
/* helper function for next3_snapshot_take() */
extern void next3_snapshot_copy_buffer(struct buffer_head *sbh,
					   struct buffer_head *bh,
					   const char *mask);
/* helper function for next3_snapshot_get_block() */
extern int next3_snapshot_read_block_bitmap(struct super_block *sb,
		unsigned int block_group, struct buffer_head *bitmap_bh);
#endif

/*
 * Snapshot constructor/destructor
 */
extern int next3_snapshot_load(struct super_block *sb,
		struct next3_super_block *es, int read_only);
extern void next3_snapshot_destroy(struct super_block *sb);
extern void next3_snapshot_update(struct super_block *sb, int cleanup);


/* balloc.c */
extern struct buffer_head *read_block_bitmap(struct super_block *sb,
					     unsigned int block_group);
#ifdef CONFIG_NEXT3_FS_SNAPSHOT_EXCLUDE_BITMAP
extern struct buffer_head *read_exclude_bitmap(struct super_block *sb,
					       unsigned int block_group);
#endif

/* namei.c */
extern int next3_inode_list_add(handle_t *handle, struct inode *inode,
				struct list_head *s_list, __le32 *s_last,
				const char *name);
extern int next3_inode_list_del(handle_t *handle, struct inode *inode,
				struct list_head *s_list, __le32 *s_last,
				const char *name);

/* inode.c */
extern next3_fsblk_t next3_get_inode_block(struct super_block *sb,
					   unsigned long ino,
					   struct next3_iloc *iloc);
#ifdef CONFIG_NEXT3_FS_SNAPSHOT_FILE_EXCLUDE
extern void next3_free_branches_cow(handle_t *handle, struct inode *inode,
				    struct buffer_head *parent_bh,
				    __le32 *first, __le32 *last,
				    int depth, int *pblocks);

#define next3_free_branches(handle, inode, bh, first, last, depth)	\
	next3_free_branches_cow((handle), (inode), (bh),		\
				(first), (last), (depth), NULL)
#endif
#ifdef CONFIG_NEXT3_FS_SNAPSHOT_FILE_SHRINK
extern int next3_snapshot_shrink_blocks(handle_t *handle,
		struct inode *start, struct inode *end,
		sector_t iblock, unsigned long maxblocks,
		struct buffer_head *cow_bh);
#endif
#ifdef CONFIG_NEXT3_FS_SNAPSHOT_FILE_MERGE
extern int next3_snapshot_merge_blocks(handle_t *handle,
		struct inode *src, struct inode *dst,
		sector_t iblock, unsigned long maxblocks);
#endif

/* super.c */
#ifdef CONFIG_NEXT3_FS_SNAPSHOT_BALLOC_RESERVE
struct kstatfs;
extern int next3_statfs_sb(struct super_block *sb, struct kstatfs *buf);
#endif

#ifdef CONFIG_NEXT3_FS_SNAPSHOT_FILE
static inline int next3_snapshot_file(struct inode *inode)
{
	return (NEXT3_I(inode)->i_flags & NEXT3_SNAPFILE_FL);
}
#endif

#ifdef CONFIG_NEXT3_FS_SNAPSHOT_EXCLUDE_INODE
static inline int next3_snapshot_exclude_inode(struct inode *inode)
{
	return (inode->i_ino == NEXT3_EXCLUDE_INO);
}
#endif

/*
 * Next3_snapshot_excluded():
 * Checks if the file should be excluded from snapshot.
 *
 * Returns 0 for normal file.
 * Returns < 0 for 'ignored' file.
 * Returns > 0 for 'excluded' file.
 *
 * Excluded and ignored file blocks are not moved to snapshot.
 * Ignored file metadata blocks are not COWed to snapshot.
 * Excluded file metadata blocks are zeroed in the snapshot file.
 * XXX: Excluded files code is experimental,
 *      but ignored files code isn't.
 */
static inline int next3_snapshot_excluded(struct inode *inode)
{
	if (!inode || !S_ISREG(inode->i_mode))
		return 0;
#ifdef CONFIG_NEXT3_FS_SNAPSHOT_FILE
	if (next3_snapshot_file(inode))
		/* ignore snapshot file */
		return -1;
#endif
#ifdef CONFIG_NEXT3_FS_SNAPSHOT_EXCLUDE_INODE
	if (next3_snapshot_exclude_inode(inode))
		/* ignore exclude inode */
		return -1;
#endif
#ifdef CONFIG_NEXT3_FS_SNAPSHOT_EXCLUDE_FILES
	/* XXX: Experimental code */
	if (NEXT3_I(inode)->i_flags & NEXT3_FL_SNAPSHOT_MASK)
		/* exclude zombie and deleted snapshot files */
		return 1;
	if (NEXT3_I(inode)->i_flags & NEXT3_NOSNAP_FL)
		/* exclude file with 'nosnap'/'nodump' flag */
		return 1;
#endif
	return 0;
}

#ifdef CONFIG_NEXT3_FS_SNAPSHOT_HOOKS_MOVE
static inline int next3_snapshot_should_move_data(struct inode *inode)
{
	if (next3_snapshot_excluded(inode))
		return 0;
	/* when data is journaled, it is already COWed as metadata */
	if (next3_should_journal_data(inode))
		return 0;
	return 1;
}
#endif

/*
 * tests if the file system has an active snapshot and returns its inode.
 * active snapshot is only changed under journal_lock_updates(),
 * so it is safe to use the returned inode during a transaction.
 */
static inline struct inode *next3_snapshot_has_active(struct super_block *sb)
{
	return NEXT3_SB(sb)->s_active_snapshot;
}

/*
 * tests if @inode is the current active snapshot.
 * active snapshot is only changed under journal_lock_updates(),
 * so the test result never changes during a transaction.
 */
static inline int next3_snapshot_is_active(struct inode *inode)
{
	return (inode == NEXT3_SB(inode->i_sb)->s_active_snapshot);
}

#ifdef CONFIG_NEXT3_FS_SNAPSHOT_RACE_COW
/*
 * Pending COW functions
 */

/*
 * Start pending COW operation from get_blocks_handle()
 * after allocating snapshot block and before connecting it
 * to the snapshot inode.
 */
static inline void next3_snapshot_start_pending_cow(struct buffer_head *sbh)
{
	/*
	 * setting the 'new' flag on a newly allocated snapshot block buffer
	 * indicates that the COW operation is pending.
	 */
	set_buffer_new(sbh);
	/* keep buffer in cache as long as we need to test the 'new' flag */
	get_bh(sbh);
}

/*
 * End pending COW operation started in get_blocks_handle().
 * Called on failure to connect the new snapshot block to the inode
 * or on successful completion of the COW operation.
 */
static inline void next3_snapshot_end_pending_cow(struct buffer_head *sbh)
{
	/*
	 * clearing the 'new' flag from the snapshot block buffer
	 * indicates that the COW operation is complete.
	 */
	clear_buffer_new(sbh);
	/* we no longer need to keep the buffer in cache */
	put_bh(sbh);
}

/*
 * Test for pending COW operation and wait for its completion.
 */
static inline void next3_snapshot_test_pending_cow(struct buffer_head *sbh,
						sector_t blocknr)
{
	SNAPSHOT_DEBUG_ONCE;
	while (buffer_new(sbh)) {
		/* wait for pending COW to complete */
		snapshot_debug_once(2, "waiting for pending cow: "
				    "block = [%lld/%lld]...\n",
				    SNAPSHOT_BLOCK_GROUP_OFFSET(blocknr),
				    SNAPSHOT_BLOCK_GROUP(blocknr));
		/*
		 * can happen once per block/snapshot - wait for COW and
		 * keep trying
		 */
		wait_on_buffer(sbh);
		yield();
	}
}
#endif

#endif	/* _LINUX_NEXT3_SNAPSHOT_H */
