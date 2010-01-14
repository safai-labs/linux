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
 * negative addresses are used to refernce snapshot meta blocks
 */
#define next3_snapblk_t long long

/*
 * We assert that snapshot must use a file system with block size == page
 * size.  Snapshot inode direct blocks are reserved for snapshot meta
 * blocks.  Snapshot inode single indirect blocks are reserved for snapshot
 * data hiding.  Snapshot image starts at the first double indirect block.
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
#define SNAPSHOT_META_HEADER	0 /* snapshot header */
#define SNAPSHOT_META_ZERO	1 /* all 'deleted' blocks point here */
#define SNAPSHOT_META_DIND	2 /* dind is hidden here */
#define SNAPSHOT_META_TIND	3 /* tind is hidden here */
#define SNAPSHOT_META_BLOCKS	4

#define SNAPSHOT_ZERO_BLOCK(inode)			\
	SNAPSHOT_META_BLOCK(inode, SNAPSHOT_META_ZERO)

#define SNAPSHOT_META_SIZE					\
	(SNAPSHOT_META_BLOCKS << SNAPSHOT_BLOCK_SIZE_BITS)
#define SNAPSHOT_SET_SIZE(inode, size)				\
	(NEXT3_I(inode)->i_disksize = SNAPSHOT_ISIZE(size))
#define SNAPSHOT_SIZE(inode)					\
	(NEXT3_I(inode)->i_disksize - SNAPSHOT_BYTES_OFFSET)
#define SNAPSHOT_BLOCKS(inode)						\
	(SNAPSHOT_SIZE(inode) >> SNAPSHOT_BLOCK_SIZE_BITS)
#define SNAPSHOT_SET_ENABLED(inode)				\
	((inode)->i_size = NEXT3_I(inode)->i_disksize)
#define SNAPSHOT_SET_DISABLED(inode)		\
	((inode)->i_size = SNAPSHOT_META_SIZE)

/* maximum recursion level allowed in snapshot file updates */
#define SNAPSHOT_MAX_RECURSION_LEVEL	2

/* block access command codes */
	/* only test if the block is in use by the active snapshot */
#define SNAPSHOT_READ	0
	/* if in use, allocate a private block for the active snapshot */
#define SNAPSHOT_WRITE	1
	/* if in use, copy the block to the active snapshot */
#define SNAPSHOT_COPY	2
	/* same as copy, but don't use journal credits */
#define SNAPSHOT_BITMAP	3
	/* if in use, move the block to the active snapshot */
#define SNAPSHOT_MOVE	-1
	/* mark the block excluded from snapshot */
#define SNAPSHOT_CLEAR	-2

/* block access return codes */
#define SNAPSHOT_FAIL	-1 /* error */
#define SNAPSHOT_OK	0 /* the block is not in use by the active snapshot */
#define SNAPSHOT_COW	1 /* the block is in use by the active snapshot */
#define SNAPSHOT_MOVED	2 /* the block was moved to the active snapshot */
#define SNAPSHOT_COPIED	3 /* the block was copied to the active snapshot */

/*
 * Block access functions
 */
#ifdef CONFIG_NEXT3_FS_SNAPSHOT_FILE
extern int next3_snapshot_get_inode_access(handle_t *handle,
					   struct inode *inode,
					   next3_fsblk_t iblock,
					   int count, int cmd);
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
					  next3_fsblk_t block, int count);
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

/* helper functions for next3_snapshot_create() */
extern int next3_snapshot_map_blocks(handle_t *handle, struct inode *inode,
				     next3_snapblk_t block,
				     unsigned long maxblocks,
				     next3_fsblk_t *mapped, int cmd);
extern int next3_snapshot_zero_data_buffer(handle_t *handle,
					   struct inode *inode,
					   next3_snapblk_t blk,
					   next3_fsblk_t blocknr);
/* helper function for next3_snapshot_take() */
extern int next3_snapshot_copy_buffer_sync(struct buffer_head *sbh,
					   struct buffer_head *bh,
					   const char *mask);
#endif

/*
 * Snapshot constructor/destructor
 */
extern void next3_snapshot_load(struct super_block *sb,
				struct next3_super_block *es);
extern void next3_snapshot_destroy(struct super_block *sb);
extern void next3_snapshot_update(struct super_block *sb, int cleanup);


#ifdef CONFIG_NEXT3_FS_SNAPSHOT_DEBUG
/* next3_debug.c */
extern const char *snapshot_cmd_str(int cmd);
extern const char *snapshot_ret_str(int ret);
extern int init_next3_snapshot(void);
extern void exit_next3_snapshot(void);
#else
static inline int init_next3_snapshot(void) { return 0; }
static inline void exit_next3_snapshot(void) { return; }
#endif

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
				    int depth, int cow);

#define next3_free_branches(handle, inode, bh, first, last, depth)	\
	next3_free_branches_cow((handle), (inode), (bh),		\
				(first), (last), (depth), 0)
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

static inline int next3_snapshot_file(struct inode *inode)
{
#ifdef CONFIG_NEXT3_FS_SNAPSHOT_FILE
	if (NEXT3_I(inode)->i_flags & NEXT3_SNAPFILE_FL)
		return 1;
#endif
	return 0;
}

static inline int next3_snapshot_exclude_inode(struct inode *inode)
{
	if (!inode)
		return 0;
#ifdef CONFIG_NEXT3_FS_SNAPSHOT_EXCLUDE_INODE
	if (inode->i_ino == NEXT3_EXCLUDE_INO)
		return 1;
#endif
	return 0;
}

/*
 * Next3_snapshot_excluded():
 * Checks if the file should be excluded from snapshot.
 * Returns non-zero for excluded file.
 * Returns < 0 for ignored file.
 *
 * Excluded/ignored file blocks are not moved to snapshot.
 * Ignored file metadata blocks are not COWed to snapshot.
 * Excluded file metadata blocks are zeroed in the snapshot file.
 */
static inline int next3_snapshot_excluded(struct inode *inode)
{
	if (!inode || !S_ISREG(inode->i_mode))
		return 0;
	if (next3_snapshot_file(inode))
		/* ignore snapshot file */
		return -1;
#ifdef CONFIG_NEXT3_FS_SNAPSHOT_EXCLUDE_INODE
	if (next3_snapshot_exclude_inode(inode))
		/* ignore exclude inode */
		return -1;
#endif
#ifdef CONFIG_NEXT3_FS_SNAPSHOT_EXCLUDE_FILES
	if (NEXT3_I(inode)->i_flags & NEXT3_FL_SNAPSHOT_MASK)
		/* exclude zombie and deleted snapshot files */
		return 1;
	if (NEXT3_I(inode)->i_flags & NEXT3_NOSNAP_FL)
		/* exclude file with 'nosnap'/'nodump' flag */
		return 1;
#endif
	return 0;
}

static inline int next3_snapshot_should_cow_data(struct inode *inode)
{
#ifdef CONFIG_NEXT3_FS_SNAPSHOT_HOOKS_MOVE
	if (next3_snapshot_excluded(inode))
		return 0;
	/* when data is journalled, it is already COWed as metadata */
	if (!next3_should_journal_data(inode))
		return 1;
#endif
	return 0;
}

/*
 * next3_snapshot_get_active() gets the current active snapshot.
 * active snapshot is only changed under journal_lock_updates(),
 * so it should be safe to use it during a transaction
 */
static inline struct inode *next3_snapshot_get_active(struct super_block *sb)
{
	return NEXT3_SB(sb)->s_active_snapshot;
}

/*
 * next3_snapshot_is_active() tests if inode is the current active snapshot.
 * the test result is valid only at the time of the test
 */
static inline int next3_snapshot_is_active(struct inode *inode)
{
	return (inode == NEXT3_SB(inode->i_sb)->s_active_snapshot);
}

/*
 * next3_snapshot_hide() hides snapshot blocks from ext2/fsck
 */
static inline void next3_snapshot_hide(struct next3_inode *raw_inode)
{
	raw_inode->i_block[SNAPSHOT_META_DIND] =
		raw_inode->i_block[NEXT3_DIND_BLOCK];
	raw_inode->i_block[SNAPSHOT_META_TIND] =
		raw_inode->i_block[NEXT3_TIND_BLOCK];
	raw_inode->i_block[NEXT3_DIND_BLOCK] = 0;
	raw_inode->i_block[NEXT3_TIND_BLOCK] = 0;
}

/*
 * next3_snapshot_unhide() un-hides snapshot blocks from ext2/fsck
 */
static inline void next3_snapshot_unhide(struct next3_inode_info *ei)
{
	if (ei->i_data[SNAPSHOT_META_DIND])
		ei->i_data[NEXT3_DIND_BLOCK] = ei->i_data[SNAPSHOT_META_DIND];
	if (ei->i_data[SNAPSHOT_META_TIND])
		ei->i_data[NEXT3_TIND_BLOCK] = ei->i_data[SNAPSHOT_META_TIND];
	ei->i_data[SNAPSHOT_META_DIND] = 0;
	ei->i_data[SNAPSHOT_META_TIND] = 0;
}

#ifdef CONFIG_NEXT3_FS_SNAPSHOT_EXCLUDE_INODE
/*
 * next3_snapshot_exclude_hide() hides exclude inode indirect blocks
 */
static inline void next3_snapshot_exclude_hide(struct next3_inode *raw_inode)
{
	raw_inode->i_block[NEXT3_IND_BLOCK] = 0;
}

/*
 * next3_snapshot_exclude_expose() exposes exclude inode indirect blocks
 */
static inline void next3_snapshot_exclude_expose(struct next3_inode_info *ei)
{
	/*
	 * Link the DIND branch to the IND branch,
	 * so we can read exclude bitmap block addresses with next3_bread().
	 *
	 * My reasons to justify this hack are:
	 * 1. I like shortcuts and it helped keeping my patch small
	 * 2. No user has access to the exclude inode
	 * 3. The exclude inode is never truncated on a mounted next3
	 * 4. The 'expose' is only to the in-memory inode (so fsck is happy)
	 * 5. A healthy exclude inode has blocks only on the DIND brnach
	 * XXX: is that a problem?
	 */
	ei->i_data[NEXT3_IND_BLOCK] = ei->i_data[NEXT3_DIND_BLOCK];
}
#endif

#ifdef CONFIG_NEXT3_FS_SNAPSHOT_RACE_COW
/*
 * Pending COW functions
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

static inline void next3_snapshot_cancel_pending_cow(struct buffer_head *sbh)
{
	/*
	 * clearing the 'new' flag from the snapshot block buffer
	 * indicates that the COW operation is complete.
	 */
	clear_buffer_new(sbh);
	/* we no longer need to keep the buffer in cache */
	put_bh(sbh);
}

static inline void next3_snapshot_test_pending_cow(struct buffer_head *sbh,
						struct buffer_head *bh)
{
	SNAPSHOT_DEBUG_ONCE;
	while (buffer_new(sbh)) {
		/* wait for pending COW to complete */
		snapshot_debug_once(2, "waiting for pending cow: "
				    "block = [%lld/%lld]...\n",
				    SNAPSHOT_BLOCK_GROUP_OFFSET(bh->b_blocknr),
				    SNAPSHOT_BLOCK_GROUP(bh->b_blocknr));
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
