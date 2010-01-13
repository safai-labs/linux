/*
 * next3_jbd.h
 *
 * Written by Stephen C. Tweedie <sct@redhat.com>, 1999
 *
 * Copyright 1998--1999 Red Hat corp --- All Rights Reserved
 *
 * This file is part of the Linux kernel and is made available under
 * the terms of the GNU General Public License, version 2, or at your
 * option, any later version, incorporated herein by reference.
 *
 * Next3-specific journaling extensions.
 *
 * Added snapshot support, Amir Goldstein <amir@ctera.com>, 2010
 */

#ifndef _LINUX_NEXT3_JBD_H
#define _LINUX_NEXT3_JBD_H

#include <linux/fs.h>
#include <linux/jbd.h>
#include "next3.h"
#include "snapshot_debug.h"

#define NEXT3_JOURNAL(inode)	(NEXT3_SB((inode)->i_sb)->s_journal)

/* Define the number of blocks we need to account to a transaction to
 * modify one block of data.
 *
 * We may have to touch one inode, one bitmap buffer, up to three
 * indirection blocks, the group and superblock summaries, and the data
 * block to complete the transaction.  */

#define NEXT3_SINGLEDATA_TRANS_BLOCKS	8U

/* Extended attribute operations touch at most two data buffers,
 * two bitmap buffers, and two group summaries, in addition to the inode
 * and the superblock, which are already accounted for. */

#define NEXT3_XATTR_TRANS_BLOCKS		6U

/* Define the minimum size for a transaction which modifies data.  This
 * needs to take into account the fact that we may end up modifying two
 * quota files too (one for the group, one for the user quota).  The
 * superblock only gets updated once, of course, so don't bother
 * counting that again for the quota updates. */

#define NEXT3_DATA_TRANS_BLOCKS(sb)	(NEXT3_SINGLEDATA_TRANS_BLOCKS + \
					 NEXT3_XATTR_TRANS_BLOCKS - 2 + \
					 2*NEXT3_QUOTA_TRANS_BLOCKS(sb))

/* Delete operations potentially hit one directory's namespace plus an
 * entire inode, plus arbitrary amounts of bitmap/indirection data.  Be
 * generous.  We can grow the delete transaction later if necessary. */

#define NEXT3_DELETE_TRANS_BLOCKS(sb)	(2 * NEXT3_DATA_TRANS_BLOCKS(sb) + 64)

/* Define an arbitrary limit for the amount of data we will anticipate
 * writing to any given transaction.  For unbounded transactions such as
 * write(2) and truncate(2) we can write more than this, but we always
 * start off at the maximum transaction size and grow the transaction
 * optimistically as we go. */

#define NEXT3_MAX_TRANS_DATA		64U

/* We break up a large truncate or write transaction once the handle's
 * buffer credits gets this low, we need either to extend the
 * transaction or to start a new one.  Reserve enough space here for
 * inode, bitmap, superblock, group and indirection updates for at least
 * one block, plus two quota updates.  Quota allocations are not
 * needed. */

#ifdef CONFIG_NEXT3_FS_SNAPSHOT_JOURNAL_CREDITS
/* on block write we have to journal the block itself */
#define NEXT3_WRITE_CREDITS 1
/* on snapshot block alloc we have to journal block group bitmap, exclude
   bitmap and gdb */
#define NEXT3_ALLOC_CREDITS 3
/* number of credits for COW bitmap operation (allocated blocks are not
   journalled): alloc(dind+ind+cow) = 9 */
#define NEXT3_COW_BITMAP_CREDITS	(3*NEXT3_ALLOC_CREDITS)
/* number of credits for other block COW operations:
   alloc(ind+cow)+write(dind+ind) = 8 */
#define NEXT3_COW_BLOCK_CREDITS	(2*NEXT3_ALLOC_CREDITS+2*NEXT3_WRITE_CREDITS)
/* number of credits for the first COW operation in the block group:
   9+8 = 17 */
#define NEXT3_COW_CREDITS	(NEXT3_COW_BLOCK_CREDITS +	\
				 NEXT3_COW_BITMAP_CREDITS)
/* number of credits for snapshot operations counted once per transaction:
   write(sb+inode+tind) = 3 */
#define NEXT3_SNAPSHOT_CREDITS 	(3*NEXT3_WRITE_CREDITS)
/*
 * in total, for N COW operations, we may have to journal 17N+3 blocks,
 * and we also want to reserve 17+3 credits for the last COW opertation,
 * so we add 17(N-1)+3+(17+3) to the requested N buffer credits
 * and request 18N+6 buffer credits.
 *
 * we are going to need a bigger journal to accomodate the
 * extra snapshot credits.
 * mke2fs uses the following default formula for fs-size above 1G:
 * journal-size = MIN(128M, fs-size/32)
 * use the following formula and override the default (-J size=):
 * journal-size = MIN(2G, fs-size/32)
 */
#define NEXT3_SNAPSHOT_TRANS_BLOCKS(n) \
	((n)*(1+NEXT3_COW_CREDITS)+NEXT3_SNAPSHOT_CREDITS)
#define NEXT3_SNAPSHOT_START_TRANS_BLOCKS(n) \
	((n)*(1+NEXT3_COW_CREDITS)+2*NEXT3_SNAPSHOT_CREDITS)

/*
 * check for sufficient buffer and COW credits
 */
#define NEXT3_SNAPSHOT_HAS_TRANS_BLOCKS(handle, n)			\
	((handle)->h_buffer_credits >= NEXT3_SNAPSHOT_TRANS_BLOCKS(n) && \
	 (handle)->h_user_credits >= (n))

#define NEXT3_RESERVE_COW_CREDITS	(NEXT3_COW_CREDITS +		\
					 NEXT3_SNAPSHOT_CREDITS)
#endif

#define NEXT3_RESERVE_TRANS_BLOCKS	12U

#define NEXT3_INDEX_EXTRA_TRANS_BLOCKS	8

#ifdef CONFIG_QUOTA
/* Amount of blocks needed for quota update - we know that the structure was
 * allocated so we need to update only inode+data */
#define NEXT3_QUOTA_TRANS_BLOCKS(sb) (test_opt(sb, QUOTA) ? 2 : 0)
/* Amount of blocks needed for quota insert/delete - we do some block writes
 * but inode, sb and group updates are done only once */
#define NEXT3_QUOTA_INIT_BLOCKS(sb) (test_opt(sb, QUOTA) ? (DQUOT_INIT_ALLOC*\
		(NEXT3_SINGLEDATA_TRANS_BLOCKS-3)+3+DQUOT_INIT_REWRITE) : 0)
#define NEXT3_QUOTA_DEL_BLOCKS(sb) (test_opt(sb, QUOTA) ? (DQUOT_DEL_ALLOC*\
		(NEXT3_SINGLEDATA_TRANS_BLOCKS-3)+3+DQUOT_DEL_REWRITE) : 0)
#else
#define NEXT3_QUOTA_TRANS_BLOCKS(sb) 0
#define NEXT3_QUOTA_INIT_BLOCKS(sb) 0
#define NEXT3_QUOTA_DEL_BLOCKS(sb) 0
#endif

int
next3_mark_iloc_dirty(handle_t *handle,
		     struct inode *inode,
		     struct next3_iloc *iloc);

/*
 * On success, We end up with an outstanding reference count against
 * iloc->bh.  This _must_ be cleaned up later.
 */

int next3_reserve_inode_write(handle_t *handle, struct inode *inode,
			struct next3_iloc *iloc);

int next3_mark_inode_dirty(handle_t *handle, struct inode *inode);

/*
 * Wrapper functions with which next3 calls into JBD.  The intent here is
 * to allow these to be turned into appropriate stubs so next3 can control
 * ext2 filesystems, so ext2+next3 systems only nee one fs.  This work hasn't
 * been done yet.
 */

#ifdef CONFIG_NEXT3_FS_SNAPSHOT_JOURNAL_RELEASE
int __next3_journal_release_buffer(const char *where, handle_t *handle,
				struct buffer_head *bh);

#define next3_journal_release_buffer(handle, bh) \
	__next3_journal_release_buffer(__func__, (handle), (bh))

#else
static inline void next3_journal_release_buffer(handle_t *handle,
						struct buffer_head *bh)
{
	journal_release_buffer(handle, bh);
}
#endif

void next3_journal_abort_handle(const char *caller, const char *err_fn,
		struct buffer_head *bh, handle_t *handle, int err);

int __next3_journal_get_undo_access(const char *where, handle_t *handle,
				struct buffer_head *bh);

#ifdef CONFIG_NEXT3_FS_SNAPSHOT_HOOKS_INODE
int __next3_journal_get_write_access_inode(const char *where, handle_t *handle,
				struct inode *inode, struct buffer_head *bh);
#else
int __next3_journal_get_write_access(const char *where, handle_t *handle,
				struct buffer_head *bh);
#endif

int __next3_journal_forget(const char *where, handle_t *handle,
				struct buffer_head *bh);

int __next3_journal_revoke(const char *where, handle_t *handle,
				unsigned long blocknr, struct buffer_head *bh);

int __next3_journal_get_create_access(const char *where,
				handle_t *handle, struct buffer_head *bh);

int __next3_journal_dirty_metadata(const char *where,
				handle_t *handle, struct buffer_head *bh);

#define next3_journal_get_undo_access(handle, bh) \
	__next3_journal_get_undo_access(__func__, (handle), (bh))
#ifdef CONFIG_NEXT3_FS_SNAPSHOT_HOOKS_INODE
#define next3_journal_get_write_access(handle, bh) \
	__next3_journal_get_write_access_inode(__func__, (handle), NULL, (bh))
#define next3_journal_get_write_access_inode(handle, inode, bh) \
	__next3_journal_get_write_access_inode(__func__, (handle), (inode), \
					       (bh))
#else
#define next3_journal_get_write_access(handle, bh) \
	__next3_journal_get_write_access(__func__, (handle), (bh))
#endif
#define next3_journal_revoke(handle, blocknr, bh) \
	__next3_journal_revoke(__func__, (handle), (blocknr), (bh))
#define next3_journal_get_create_access(handle, bh) \
	__next3_journal_get_create_access(__func__, (handle), (bh))
#define next3_journal_dirty_metadata(handle, bh) \
	__next3_journal_dirty_metadata(__func__, (handle), (bh))
#define next3_journal_forget(handle, bh) \
	__next3_journal_forget(__func__, (handle), (bh))

int next3_journal_dirty_data(handle_t *handle, struct buffer_head *bh);

#ifdef CONFIG_NEXT3_FS_SNAPSHOT_JOURNAL_TRACE
void __next3_journal_trace(int debug, const char *fn, const char *caller,
		handle_t *handle, int nblocks);

#define next3_journal_trace(n, caller, handle, nblocks)			\
	do {								\
		if ((n) <= snapshot_enable_debug)			\
			__next3_journal_trace((n), __func__, (caller),	\
					      (handle), (nblocks));	\
	} while (0)

#else
#define next3_journal_trace(n, caller, handle, nblocks)
#endif

#ifdef CONFIG_NEXT3_FS_SNAPSHOT_JOURNAL_CREDITS
handle_t *__next3_journal_start(const char *where,
		struct super_block *sb, int nblocks);

#define next3_journal_start_sb(sb, nblocks) \
	__next3_journal_start(__func__, 	\
			(sb), (nblocks))

#define next3_journal_start(inode, nblocks) \
	__next3_journal_start(__func__, 	\
			(inode)->i_sb, (nblocks))

#else
handle_t *next3_journal_start_sb(struct super_block *sb, int nblocks);

static inline handle_t *next3_journal_start(struct inode *inode, int nblocks)
{
	return next3_journal_start_sb(inode->i_sb, nblocks);
}
#endif

int __next3_journal_stop(const char *where, handle_t *handle);

#define next3_journal_stop(handle) \
	__next3_journal_stop(__func__, (handle))

static inline handle_t *next3_journal_current_handle(void)
{
	return journal_current_handle();
}

#ifdef CONFIG_NEXT3_FS_SNAPSHOT_JOURNAL_CREDITS
static inline int __next3_journal_extend(const char *where,
		handle_t *handle, int nblocks)
{
	int lower = NEXT3_SNAPSHOT_TRANS_BLOCKS(handle->h_user_credits+nblocks);
	int err = 0;
	int missing = lower - handle->h_buffer_credits;
	if (missing > 0)
		/* extend transaction to keep buffer credits above lower
		 * limit */
		err = journal_extend(handle, missing);
	if (!err) {
		handle->h_base_credits += nblocks;
		handle->h_user_credits += nblocks;
		next3_journal_trace(SNAP_WARN, where, handle, nblocks);
	}
	return err;
}

static inline int __next3_journal_restart(const char *where,
					  handle_t *handle, int nblocks)
{
	int err = journal_restart(handle,
				  NEXT3_SNAPSHOT_START_TRANS_BLOCKS(nblocks));
	if (!err) {
		handle->h_base_credits = nblocks;
		handle->h_user_credits = nblocks;
		next3_journal_trace(SNAP_WARN, where, handle, nblocks);
	}
	return err;
}


#define next3_journal_extend(handle, nblocks) \
	__next3_journal_extend(__func__, 	\
			(handle), (nblocks))

#define next3_journal_restart(handle, nblocks) \
	__next3_journal_restart(__func__, 	\
			(handle), (nblocks))

#else
static inline int next3_journal_extend(handle_t *handle, int nblocks)
{
	return journal_extend(handle, nblocks);
}

static inline int next3_journal_restart(handle_t *handle, int nblocks)
{
	return journal_restart(handle, nblocks);
}
#endif

static inline int next3_journal_blocks_per_page(struct inode *inode)
{
	return journal_blocks_per_page(inode);
}

static inline int next3_journal_force_commit(journal_t *journal)
{
	return journal_force_commit(journal);
}

/* super.c */
int next3_force_commit(struct super_block *sb);

static inline int next3_should_journal_data(struct inode *inode)
{
	if (!S_ISREG(inode->i_mode))
		return 1;
	if (test_opt(inode->i_sb, DATA_FLAGS) == NEXT3_MOUNT_JOURNAL_DATA)
		return 1;
	if (NEXT3_I(inode)->i_flags & NEXT3_JOURNAL_DATA_FL)
		return 1;
	return 0;
}

static inline int next3_should_order_data(struct inode *inode)
{
	if (!S_ISREG(inode->i_mode))
		return 0;
	if (NEXT3_I(inode)->i_flags & NEXT3_JOURNAL_DATA_FL)
		return 0;
	if (test_opt(inode->i_sb, DATA_FLAGS) == NEXT3_MOUNT_ORDERED_DATA)
		return 1;
	return 0;
}

static inline int next3_should_writeback_data(struct inode *inode)
{
	if (!S_ISREG(inode->i_mode))
		return 0;
	if (NEXT3_I(inode)->i_flags & NEXT3_JOURNAL_DATA_FL)
		return 0;
	if (test_opt(inode->i_sb, DATA_FLAGS) == NEXT3_MOUNT_WRITEBACK_DATA)
		return 1;
	return 0;
}

#endif	/* _LINUX_NEXT3_JBD_H */
