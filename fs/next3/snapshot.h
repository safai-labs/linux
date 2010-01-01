/*
 * linux/fs/next3/snapshot.h
 *
 * Copyright (C) 2008 Amir Goldor <amir73il@users.sf.net>
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


#define NEXT3_SNAPSHOT_VERSION "next3 snapshot v1.0.0-rc6"

/* 
 * use signed 64bit for snapshot image addresses
 * negative addresses are used to refernce snapshot meta blocks
 */
typedef long long next3_snapblk_t;

/*
 * we assert that snapshot must use a file system with block size == page size.
 * snapshot inode direct blocks are reserved for snapshot meta blocks.
 * snapshot inode single indirect blocks are reserved for snapshot data hiding.
 * snapshot image starts at the first double indirect block.
 * this way, a snapshot image block group can be mapped with
 * 1 double indirect block + 32 indirect blocks.
 * to mount the snapshot image, the loop device should be configured with:
 * '-o <SNAPSHOT_BYTES_OFFSET>' (i.e. (12+1k)*4k = 4243456)
 */
#define SNAPSHOT_BLOCK_SIZE				PAGE_SIZE
#define SNAPSHOT_BLOCK_SIZE_BITS		PAGE_SHIFT
#define	SNAPSHOT_ADDR_PER_BLOCK			(SNAPSHOT_BLOCK_SIZE / sizeof (__u32))
#define SNAPSHOT_ADDR_PER_BLOCK_BITS	(SNAPSHOT_BLOCK_SIZE_BITS - 2)
#define SNAPSHOT_DIR_BLOCKS				NEXT3_NDIR_BLOCKS
#define SNAPSHOT_IND_BLOCKS				SNAPSHOT_ADDR_PER_BLOCK	

#define SNAPSHOT_BLOCKS_PER_GROUP_BITS	15
#define SNAPSHOT_BLOCKS_PER_GROUP		(1<<SNAPSHOT_BLOCKS_PER_GROUP_BITS) /* 32K */
#define SNAPSHOT_BLOCK_GROUP(block)		((block)>>SNAPSHOT_BLOCKS_PER_GROUP_BITS)
#define SNAPSHOT_BLOCK_GROUP_OFFSET(block)	((block)&(SNAPSHOT_BLOCKS_PER_GROUP-1))
#define SNAPSHOT_IND_PER_BLOCK_GROUP_BITS	(SNAPSHOT_BLOCKS_PER_GROUP_BITS-SNAPSHOT_ADDR_PER_BLOCK_BITS)
#define SNAPSHOT_IND_PER_BLOCK_GROUP	(1<<SNAPSHOT_IND_PER_BLOCK_GROUP_BITS) /* 32 */
#define SNAPSHOT_DIND_BLOCK_GROUPS_BITS	(SNAPSHOT_ADDR_PER_BLOCK_BITS-SNAPSHOT_IND_PER_BLOCK_GROUP_BITS)
#define SNAPSHOT_DIND_BLOCK_GROUPS		(1<<SNAPSHOT_DIND_BLOCK_GROUPS_BITS) /* 32 */

#define SNAPSHOT_BLOCK_OFFSET			(SNAPSHOT_DIR_BLOCKS+SNAPSHOT_IND_BLOCKS)
#define SNAPSHOT_BLOCK(iblock)			((next3_snapblk_t)(iblock) - SNAPSHOT_BLOCK_OFFSET)
#define SNAPSHOT_IBLOCK(block)			(next3_fsblk_t)((block) + SNAPSHOT_BLOCK_OFFSET)
#define SNAPSHOT_META_BLOCK(inode,iblock)	(NEXT3_I(inode)->i_data[iblock])

#define SNAPSHOT_BYTES_OFFSET			(SNAPSHOT_BLOCK_OFFSET << SNAPSHOT_BLOCK_SIZE_BITS)
#define SNAPSHOT_ISIZE(size)			((size) + SNAPSHOT_BYTES_OFFSET)

/*
 * snapshot meta blocks:
 */
#define SNAPSHOT_META_HEADER	0 /* snapshot header */
#define SNAPSHOT_META_ZERO		1 /* all 'deleted' blocks point here */
#define SNAPSHOT_META_DIND		2 /* dind is hidden here */
#define SNAPSHOT_META_TIND		3 /* tind is hidden here */
#define SNAPSHOT_META_BLOCKS	4

#define SNAPSHOT_ZERO_BLOCK(inode) 		SNAPSHOT_META_BLOCK(inode,SNAPSHOT_META_ZERO)

#define SNAPSHOT_META_SIZE				(SNAPSHOT_META_BLOCKS << SNAPSHOT_BLOCK_SIZE_BITS)
#define SNAPSHOT_SET_SIZE(inode,size)	(NEXT3_I(inode)->i_disksize = SNAPSHOT_ISIZE(size))
#define SNAPSHOT_SIZE(inode)			(NEXT3_I(inode)->i_disksize - SNAPSHOT_BYTES_OFFSET)
#define SNAPSHOT_BLOCKS(inode)			(SNAPSHOT_SIZE(inode) >> SNAPSHOT_BLOCK_SIZE_BITS)
#define SNAPSHOT_SET_ENABLED(inode)		((inode)->i_size = NEXT3_I(inode)->i_disksize)
#define SNAPSHOT_SET_DISABLED(inode)	((inode)->i_size = SNAPSHOT_META_SIZE)

/* maximum recursion level allowed in snapshot file updates */
#define SNAPSHOT_MAX_RECURSION_LEVEL	2

/* block access command codes */
#define SNAPSHOT_READ	0 /* only test if the block is in use by the active snapshot */
#define SNAPSHOT_WRITE	1 /* if in use, allocate a private block for the active snapshot */
#define SNAPSHOT_COPY	2 /* if in use, copy the block to the active snapshot */
#define SNAPSHOT_BITMAP	3 /* same as copy, but don't use journal credits */
#define SNAPSHOT_MOVE	-1 /* if in use, move the block to the active snapshot */
#define SNAPSHOT_DEL	-2 /* if in use, mark the block deleted in the active snapshot */
#define SNAPSHOT_SHRINK	-3 /* free unused blocks of deleted snapshots */

/* block access return codes */
#define SNAPSHOT_FAIL	-1 /* error */
#define SNAPSHOT_OK		0 /* the block is not in use by the active snapshot */
#define SNAPSHOT_COW	1 /* the block is in use by the active snapshot */
#define SNAPSHOT_MOVED	2 /* the block was moved to the active snapshot */
#define SNAPSHOT_COPIED	3 /* the block was copied to the active snapshot */

/*
 * Block access functions
 */
#ifdef CONFIG_NEXT3_FS_SNAPSHOT_FILE
int next3_snapshot_get_inode_access(handle_t *handle, struct inode *inode,
									next3_fsblk_t iblock, int count, int cmd);
#endif
#ifdef CONFIG_NEXT3_FS_SNAPSHOT_HOOKS
int next3_snapshot_get_undo_access(handle_t *handle, struct buffer_head *bh);
int next3_snapshot_get_write_access(handle_t *handle, struct inode *inode,
													struct buffer_head *bh);
int next3_snapshot_get_create_access(handle_t *handle, struct buffer_head *bh);
int next3_snapshot_get_move_access(handle_t *handle, struct inode *inode, 
										next3_fsblk_t block, int count);
int next3_snapshot_get_delete_access(handle_t *handle, struct inode *inode,
										next3_fsblk_t block, int count);
#endif
#ifdef CONFIG_NEXT3_FS_SNAPSHOT_FILE_EXCLUDE
int next3_snapshot_get_clear_access(handle_t *handle, struct inode *inode,
										next3_fsblk_t block, int count);
#endif

#ifdef CONFIG_NEXT3_FS_SNAPSHOT_CTL
/*
 * Snapshot control functions
 */
void next3_snapshot_get_flags(struct next3_inode_info *ei, struct file *filp);
int next3_snapshot_set_flags(handle_t *handle, struct inode *inode, unsigned int flags);
int next3_snapshot_create(struct inode *inode);
int next3_snapshot_take(struct inode *inode);
int next3_snapshot_enable(struct inode *inode);
int next3_snapshot_disable(struct inode *inode);
int next3_snapshot_delete(struct inode *inode);
#ifdef CONFIG_NEXT3_FS_SNAPSHOT_DEBUG
void next3_snapshot_dump(struct inode *inode);
#endif
#ifdef CONFIG_NEXT3_FS_SNAPSHOT_FILE_EXCLUDE
int next3_snapshot_clean(handle_t *handle, struct inode *inode);
#endif
#ifdef CONFIG_NEXT3_FS_SNAPSHOT_BALLOC_SAFE
int next3_snapshot_stabilize(handle_t *handle, struct inode *inode);
int next3_snapshot_verify(handle_t *handle, struct inode *inode);
#endif
#endif

/*
 * Snapshot constructor/destructor
 */
void next3_snapshot_load(struct super_block *sb, struct next3_super_block *es);
void next3_snapshot_destroy(struct super_block *sb);
void next3_snapshot_update(struct super_block *sb, int cleanup);


#ifdef CONFIG_NEXT3_FS_SNAPSHOT_DEBUG
/* next3_debug.c */
const char *snapshot_cmd_str(int cmd);
const char *snapshot_ret_str(int ret);
int init_next3_snapshot(void);
void exit_next3_snapshot(void);
#else
inline static int init_next3_snapshot(void) {return 0;}
inline static void exit_next3_snapshot(void) {}
#endif

/* namei.c */
int next3_inode_list_add(handle_t *handle, struct inode *inode, 
		struct list_head *s_list, __le32 *s_last, const char *name);
int next3_inode_list_del(handle_t *handle, struct inode *inode, 
		struct list_head *s_list, __le32 *s_last, const char *name);

/* inode.c */
next3_fsblk_t next3_get_inode_block(struct super_block *sb,
		unsigned long ino, struct next3_iloc *iloc);
#ifdef CONFIG_NEXT3_FS_SNAPSHOT_FILE_EXCLUDE
void next3_free_branches_cow(handle_t *handle, struct inode *inode,
			       struct buffer_head *parent_bh,
			       __le32 *first, __le32 *last, int depth, int cow);
#endif
#ifdef CONFIG_NEXT3_FS_SNAPSHOT_FILE_MERGE
int next3_merge_blocks(handle_t *handle, struct inode *src, struct inode *dst, 
		sector_t iblock, unsigned long maxblocks);
#endif

#ifdef CONFIG_NEXT3_FS_SNAPSHOT_HOOKS_MOVE
#define next3_get_branch_cow(handle, inode, depth, offsets, chain, err, cmd) \
	__next3_get_branch_cow(__func__, handle, inode, depth, offsets, chain, err, cmd)
#define next3_get_branch(inode, depth, offsets, chain, err) \
	__next3_get_branch_cow(__func__, NULL, inode, depth, offsets, chain, err, 0)
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

/* 
 * excluded file blocks are not COWed to snapshot 
 * and they are marked deleted in the snapshot file.
 * snapshot file blocks are only cleared from COW bitmap.
 */
static inline int next3_snapshot_excluded(struct inode *inode)
{
	if (!inode || !S_ISREG(inode->i_mode))
		return 0;
#ifdef CONFIG_NEXT3_FS_SNAPSHOT_FILE_EXCLUDE
	if (next3_snapshot_file(inode))
		/* clear bit from COW bitmap */
		return -1;
#endif
#ifdef CONFIG_NEXT3_FS_SNAPSHOT_EXCLUDE
	if (NEXT3_I(inode)->i_flags & NEXT3_FL_SNAPSHOT_MASK)
		/* exclude zombie and deleted snapshot files */
		return 1;
#ifdef CONFIG_NEXT3_FS_SNAPSHOT_EXCLUDE_FILES
	if (NEXT3_I(inode)->i_flags & NEXT3_NOSNAP_FL)
		/* exclude file with 'nosnap'/'nodump' flag */
		return 1;
#endif
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
	raw_inode->i_block[SNAPSHOT_META_DIND] = raw_inode->i_block[NEXT3_DIND_BLOCK];
	raw_inode->i_block[SNAPSHOT_META_TIND] = raw_inode->i_block[NEXT3_TIND_BLOCK];
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


#endif	/* _LINUX_NEXT3_SNAPSHOT_H */
