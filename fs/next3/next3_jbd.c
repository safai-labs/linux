/*
 * Interface between next3 and JBD
 */

#include "next3_jbd.h"
#include "snapshot.h"

int __next3_journal_get_undo_access(const char *where, handle_t *handle,
				struct buffer_head *bh)
{
	int err = journal_get_undo_access(handle, bh);
#ifdef CONFIG_NEXT3_FS_SNAPSHOT_HOOKS_JBD
	if (!err) {
		err = next3_snapshot_get_undo_access(handle, bh);
		if (err > 0)
			/* deny access if block should be COWed */
			err = ((err == SNAPSHOT_COW) ? -EIO : 0);
	}
#endif
	if (err)
		next3_journal_abort_handle(where, __func__, bh, handle,err);
#ifdef CONFIG_NEXT3_FS_SNAPSHOT_JOURNAL_TRACE
	next3_journal_trace(SNAP_DEBUG, where, handle, 1);
#endif
	return err;
}

#ifdef CONFIG_NEXT3_FS_SNAPSHOT_HOOKS_INODE
int __next3_journal_get_write_access_inode(const char *where, handle_t *handle,
				struct inode *inode, struct buffer_head *bh)
#else
int __next3_journal_get_write_access(const char *where, handle_t *handle,
				struct buffer_head *bh)
#endif
{
	int err = journal_get_write_access(handle, bh);
#ifdef CONFIG_NEXT3_FS_SNAPSHOT_HOOKS_JBD
	if (!err) {
		err = next3_snapshot_get_write_access(handle, inode, bh);
		if (err > 0)
			/* deny access if block should be COWed */
			err = ((err == SNAPSHOT_COW) ? -EIO : 0);
	}
#endif
	if (err)
		next3_journal_abort_handle(where, __func__, bh, handle,err);
#ifdef CONFIG_NEXT3_FS_SNAPSHOT_JOURNAL_TRACE
	next3_journal_trace(SNAP_DEBUG, where, handle, 1);
#endif
	return err;
}

int __next3_journal_forget(const char *where, handle_t *handle,
				struct buffer_head *bh)
{
	int err = journal_forget(handle, bh);
	if (err)
		next3_journal_abort_handle(where, __func__, bh, handle,err);
#ifdef CONFIG_NEXT3_FS_SNAPSHOT_JOURNAL_TRACE
	next3_journal_trace(SNAP_DEBUG, where, handle, 1);
#endif
	return err;
}

int __next3_journal_revoke(const char *where, handle_t *handle,
				unsigned long blocknr, struct buffer_head *bh)
{
	int err = journal_revoke(handle, blocknr, bh);
	if (err)
		next3_journal_abort_handle(where, __func__, bh, handle,err);
#ifdef CONFIG_NEXT3_FS_SNAPSHOT_JOURNAL_TRACE
	next3_journal_trace(SNAP_DEBUG, where, handle, 1);
#endif
	return err;
}

int __next3_journal_get_create_access(const char *where,
				handle_t *handle, struct buffer_head *bh)
{
	int err = journal_get_create_access(handle, bh);
#ifdef CONFIG_NEXT3_FS_SNAPSHOT_HOOKS_JBD
	if (!err) {
		err = next3_snapshot_get_create_access(handle, bh);
		if (err > 0)
			/* deny access if block should be COWed */
			err = ((err == SNAPSHOT_COW) ? -EIO : 0);
	}
#endif
	if (err)
		next3_journal_abort_handle(where, __func__, bh, handle,err);
#ifdef CONFIG_NEXT3_FS_SNAPSHOT_JOURNAL_TRACE
	next3_journal_trace(SNAP_DEBUG, where, handle, -1);
#endif
	return err;
}

int __next3_journal_dirty_metadata(const char *where,
				handle_t *handle, struct buffer_head *bh)
{
	int err = journal_dirty_metadata(handle, bh);
	if (err)
		next3_journal_abort_handle(where, __func__, bh, handle,err);
#ifdef CONFIG_NEXT3_FS_SNAPSHOT_JOURNAL_TRACE
	next3_journal_trace(SNAP_DEBUG, where, handle, -1);
#endif
	return err;
}

#ifdef CONFIG_NEXT3_FS_SNAPSHOT_JOURNAL_RELEASE
int __next3_journal_release_buffer(const char *where, handle_t *handle,
				struct buffer_head *bh)
{
	int err = 0;
	if (handle->h_level == 0) {
		/* 
		 * trying to cancel a previous call to get_write_access(),
		 * which may have resulted in a single COW operation.
		 * we try to extend the transaction to compensate 
		 * for the buffer credits used by the extra COW operation.
		 */
		err = next3_journal_extend(handle, 1);
		if (err > 0) {
			/* well, we can't say we didn't try - 
			 * now lets hope we have enough credits to spare */
			err = 0;
		}
		next3_journal_trace(SNAP_WARN, where, handle, -1);
	}
	journal_release_buffer(handle, bh);
	return err;
}
#endif

#ifdef CONFIG_NEXT3_FS_SNAPSHOT_JOURNAL_TRACE
void __next3_journal_trace(int n, const char *fn, const char *caller, 
		handle_t *handle, int nblocks)
{
	struct super_block *sb = handle->h_transaction->t_journal->j_private;
	struct inode *snapshot = next3_snapshot_get_active(sb);
	int upper = NEXT3_SNAPSHOT_START_TRANS_BLOCKS(handle->h_base_credits);
	int lower = NEXT3_SNAPSHOT_TRANS_BLOCKS(handle->h_cow_credits);
	int final = (nblocks == 0 && handle->h_ref == 1 && handle->h_level == 0);

	switch (snapshot_enable_debug) {
		case SNAP_INFO:
			/* trace final journal_stop if any credits have been used */
			if (final && (handle->h_buffer_credits < upper ||
				handle->h_cow_credits < handle->h_base_credits))
				break;
		case SNAP_WARN:
			/* 
			 * trace if buffer credits are too low -
			 * lower limit is only valid if there is
			 * an active snapshot and not during COW 
			 */
			if (handle->h_buffer_credits < lower &&
				snapshot && handle->h_level == 0)
				break;
		case SNAP_ERR:
			/* trace if COW credits are too low */
			if (handle->h_cow_credits < 0)
				break;
		case 0:
			/* no trace */
			return;

		case SNAP_DEBUG:
		default:
			/* trace all calls */
			break;
	}

	snapshot_debug_l(n, handle->h_level, 
			"%s(%d): credits=%d, limit=%d/%d, COW=%d/%d, ref=%d, caller=%s\n", 
			fn, nblocks, handle->h_buffer_credits, lower, upper,	
			handle->h_cow_credits, 
			handle->h_base_credits,
			handle->h_ref, caller);
	if (final)
		snapshot_debug_l(n, handle->h_level, 
				"COW stats: moved/copied=%d/%d, mapped/clear/cached=%d/%d/%d, bitmaps/cleared=%d/%d\n", 
				handle->h_cow_moved, handle->h_cow_copied,
				handle->h_cow_ok_mapped, handle->h_cow_ok_clear, handle->h_cow_ok_jh,
				handle->h_cow_bitmaps, handle->h_cow_cleared);
}
#endif

