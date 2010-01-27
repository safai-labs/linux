/*
 * linux/fs/next3/ioctl.c
 *
 * Copyright (C) 1993, 1994, 1995
 * Remy Card (card@masi.ibp.fr)
 * Laboratoire MASI - Institut Blaise Pascal
 * Universite Pierre et Marie Curie (Paris VI)
 *
 * Added snapshot support, Amir Goldstein <amir@ctera.com>, 2010
 */

#include <linux/fs.h>
#include <linux/jbd.h>
#include <linux/capability.h>
#include "next3.h"
#include "next3_jbd.h"
#include <linux/mount.h>
#include <linux/time.h>
#include <linux/compat.h>
#include <asm/uaccess.h>
#include "snapshot.h"

long next3_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
	struct inode *inode = filp->f_dentry->d_inode;
	struct next3_inode_info *ei = NEXT3_I(inode);
	unsigned int flags;
	unsigned short rsv_window_size;

	next3_debug ("cmd = %u, arg = %lu\n", cmd, arg);

	switch (cmd) {
	case NEXT3_IOC_GETFLAGS:
		next3_get_inode_flags(ei);
#ifdef CONFIG_NEXT3_FS_SNAPSHOT_CTL
		next3_snapshot_get_flags(ei, filp);
#endif
		flags = ei->i_flags & NEXT3_FL_USER_VISIBLE;
		return put_user(flags, (int __user *) arg);
	case NEXT3_IOC_SETFLAGS: {
		handle_t *handle = NULL;
		int err;
		struct next3_iloc iloc;
		unsigned int oldflags;
		unsigned int jflag;
#ifdef CONFIG_NEXT3_FS_SNAPSHOT_CTL
		unsigned int snapflags = 0;
#endif

		if (!is_owner_or_cap(inode))
			return -EACCES;

		if (get_user(flags, (int __user *) arg))
			return -EFAULT;

		err = mnt_want_write(filp->f_path.mnt);
		if (err)
			return err;

		flags = next3_mask_flags(inode->i_mode, flags);

		mutex_lock(&inode->i_mutex);

		/* Is it quota file? Do not allow user to mess with it */
		err = -EPERM;
		if (IS_NOQUOTA(inode))
			goto flags_out;

		oldflags = ei->i_flags;

		/* The JOURNAL_DATA flag is modifiable only by root */
		jflag = flags & NEXT3_JOURNAL_DATA_FL;

		/*
		 * The IMMUTABLE and APPEND_ONLY flags can only be changed by
		 * the relevant capability.
		 *
		 * This test looks nicer. Thanks to Pauline Middelink
		 */
		if ((flags ^ oldflags) & (NEXT3_APPEND_FL | NEXT3_IMMUTABLE_FL)) {
			if (!capable(CAP_LINUX_IMMUTABLE))
				goto flags_out;
		}

		/*
		 * The JOURNAL_DATA flag can only be changed by
		 * the relevant capability.
		 */
		if ((jflag ^ oldflags) & (NEXT3_JOURNAL_DATA_FL)) {
			if (!capable(CAP_SYS_RESOURCE))
				goto flags_out;
		}

#ifdef CONFIG_NEXT3_FS_SNAPSHOT_CTL
		/*
		 * Snapshot flags and snapshot file (has snapshot flags)
		 * flags (including non-snapshot flags) can only be changed by
		 * the relevant capability and under snapshot_mutex lock.
		 */
		snapflags = ((flags | oldflags) & NEXT3_FL_SNAPSHOT_MASK);
		if (snapflags) {
			if (!capable(CAP_SYS_RESOURCE)) {
				/* indicate snapshot_mutex not taken */
				snapflags = 0;
				goto flags_out;
			}

			/*
			 * snapshot_mutex should be held throughout the trio
			 * snapshot_{set_flags,take,update}().  It must be taken 
			 * before starting the transaction, otherwise
			 * journal_lock_updates() inside snapshot_take()
			 * can deadlock:
			 * A: journal_start()
			 * A: snapshot_mutex_lock()
			 * B: journal_start()
			 * B: snapshot_mutex_lock() (waiting for A)
			 * A: journal_stop()
			 * A: snapshot_take() ->
			 * A: 	journal_lock_updates() (waiting for B)
			 */
			mutex_lock(&NEXT3_SB(inode->i_sb)->s_snapshot_mutex);
		}
#endif

		handle = next3_journal_start(inode, 1);
		if (IS_ERR(handle)) {
			err = PTR_ERR(handle);
			goto flags_out;
		}
		if (IS_SYNC(inode))
			handle->h_sync = 1;
		err = next3_reserve_inode_write(handle, inode, &iloc);
		if (err)
			goto flags_err;

		flags = flags & NEXT3_FL_USER_MODIFIABLE;
		flags |= oldflags & ~NEXT3_FL_USER_MODIFIABLE;
#ifdef CONFIG_NEXT3_FS_SNAPSHOT_CTL
		err = next3_snapshot_set_flags(handle, inode, flags);
		if (err)
			goto flags_err;
#else
		ei->i_flags = flags;
#endif

		next3_set_inode_flags(inode);
		inode->i_ctime = CURRENT_TIME_SEC;

		err = next3_mark_iloc_dirty(handle, inode, &iloc);
flags_err:
		next3_journal_stop(handle);
		if (err)
			goto flags_out;

		if ((jflag ^ oldflags) & (NEXT3_JOURNAL_DATA_FL))
			err = next3_change_inode_journal_flag(inode, jflag);
#ifdef CONFIG_NEXT3_FS_SNAPSHOT_CTL
		if (err)
			goto flags_out;

		if ((flags & NEXT3_SNAPFILE_FL) &&
			(ei->i_flags & NEXT3_SNAPFILE_TAKE_FL))
			/* take snapshot outside transaction */
			err = next3_snapshot_take(inode);

		if ((flags | oldflags) & NEXT3_SNAPFILE_FL)
			/*
			 * Finally: update all snapshots status flags
			 * and cleanup after delete command
			 */
			next3_snapshot_update(inode->i_sb,
					      !(flags & NEXT3_SNAPFILE_FL));
#endif
flags_out:
#ifdef CONFIG_NEXT3_FS_SNAPSHOT_CTL
		if (snapflags)
			mutex_unlock(&NEXT3_SB(inode->i_sb)->s_snapshot_mutex);
#endif
		mutex_unlock(&inode->i_mutex);
		mnt_drop_write(filp->f_path.mnt);
		return err;
	}
	case NEXT3_IOC_GETVERSION:
	case NEXT3_IOC_GETVERSION_OLD:
		return put_user(inode->i_generation, (int __user *) arg);
	case NEXT3_IOC_SETVERSION:
	case NEXT3_IOC_SETVERSION_OLD: {
		handle_t *handle;
		struct next3_iloc iloc;
		__u32 generation;
		int err;

		if (!is_owner_or_cap(inode))
			return -EPERM;

		err = mnt_want_write(filp->f_path.mnt);
		if (err)
			return err;
		if (get_user(generation, (int __user *) arg)) {
			err = -EFAULT;
			goto setversion_out;
		}

		handle = next3_journal_start(inode, 1);
		if (IS_ERR(handle)) {
			err = PTR_ERR(handle);
			goto setversion_out;
		}
		err = next3_reserve_inode_write(handle, inode, &iloc);
		if (err == 0) {
			inode->i_ctime = CURRENT_TIME_SEC;
			inode->i_generation = generation;
			err = next3_mark_iloc_dirty(handle, inode, &iloc);
		}
		next3_journal_stop(handle);
setversion_out:
		mnt_drop_write(filp->f_path.mnt);
		return err;
	}
#ifdef CONFIG_JBD_DEBUG
	case NEXT3_IOC_WAIT_FOR_READONLY:
		/*
		 * This is racy - by the time we're woken up and running,
		 * the superblock could be released.  And the module could
		 * have been unloaded.  So sue me.
		 *
		 * Returns 1 if it slept, else zero.
		 */
		{
			struct super_block *sb = inode->i_sb;
			DECLARE_WAITQUEUE(wait, current);
			int ret = 0;

			set_current_state(TASK_INTERRUPTIBLE);
			add_wait_queue(&NEXT3_SB(sb)->ro_wait_queue, &wait);
			if (timer_pending(&NEXT3_SB(sb)->turn_ro_timer)) {
				schedule();
				ret = 1;
			}
			remove_wait_queue(&NEXT3_SB(sb)->ro_wait_queue, &wait);
			return ret;
		}
#endif
	case NEXT3_IOC_GETRSVSZ:
		if (test_opt(inode->i_sb, RESERVATION)
			&& S_ISREG(inode->i_mode)
			&& ei->i_block_alloc_info) {
			rsv_window_size = ei->i_block_alloc_info->rsv_window_node.rsv_goal_size;
			return put_user(rsv_window_size, (int __user *)arg);
		}
		return -ENOTTY;
	case NEXT3_IOC_SETRSVSZ: {
		int err;

		if (!test_opt(inode->i_sb, RESERVATION) ||!S_ISREG(inode->i_mode))
			return -ENOTTY;

		err = mnt_want_write(filp->f_path.mnt);
		if (err)
			return err;

		if (!is_owner_or_cap(inode)) {
			err = -EACCES;
			goto setrsvsz_out;
		}

		if (get_user(rsv_window_size, (int __user *)arg)) {
			err = -EFAULT;
			goto setrsvsz_out;
		}

		if (rsv_window_size > NEXT3_MAX_RESERVE_BLOCKS)
			rsv_window_size = NEXT3_MAX_RESERVE_BLOCKS;

		/*
		 * need to allocate reservation structure for this inode
		 * before set the window size
		 */
		mutex_lock(&ei->truncate_mutex);
		if (!ei->i_block_alloc_info)
			next3_init_block_alloc_info(inode);

		if (ei->i_block_alloc_info){
			struct next3_reserve_window_node *rsv = &ei->i_block_alloc_info->rsv_window_node;
			rsv->rsv_goal_size = rsv_window_size;
		}
		mutex_unlock(&ei->truncate_mutex);
setrsvsz_out:
		mnt_drop_write(filp->f_path.mnt);
		return err;
	}
	case NEXT3_IOC_GROUP_EXTEND: {
		next3_fsblk_t n_blocks_count;
		struct super_block *sb = inode->i_sb;
		int err, err2;

		if (!capable(CAP_SYS_RESOURCE))
			return -EPERM;

		err = mnt_want_write(filp->f_path.mnt);
		if (err)
			return err;

		if (get_user(n_blocks_count, (__u32 __user *)arg)) {
			err = -EFAULT;
			goto group_extend_out;
		}
#ifdef CONFIG_NEXT3_FS_SNAPSHOT_CTL
		/* avoid snapshot_take() in the middle of group_extend() */
		mutex_lock(&NEXT3_SB(sb)->s_snapshot_mutex);
#endif
		err = next3_group_extend(sb, NEXT3_SB(sb)->s_es, n_blocks_count);
		journal_lock_updates(NEXT3_SB(sb)->s_journal);
		err2 = journal_flush(NEXT3_SB(sb)->s_journal);
		journal_unlock_updates(NEXT3_SB(sb)->s_journal);
		if (err == 0)
			err = err2;
#ifdef CONFIG_NEXT3_FS_SNAPSHOT_CTL
		mutex_unlock(&NEXT3_SB(sb)->s_snapshot_mutex);
#endif
group_extend_out:
		mnt_drop_write(filp->f_path.mnt);
		return err;
	}
	case NEXT3_IOC_GROUP_ADD: {
		struct next3_new_group_data input;
		struct super_block *sb = inode->i_sb;
		int err, err2;

		if (!capable(CAP_SYS_RESOURCE))
			return -EPERM;

		err = mnt_want_write(filp->f_path.mnt);
		if (err)
			return err;

		if (copy_from_user(&input, (struct next3_new_group_input __user *)arg,
				sizeof(input))) {
			err = -EFAULT;
			goto group_add_out;
		}

#ifdef CONFIG_NEXT3_FS_SNAPSHOT_CTL
		/* avoid snapshot_take() in the middle of group_add() */
		mutex_lock(&NEXT3_SB(sb)->s_snapshot_mutex);
#endif
		err = next3_group_add(sb, &input);
		journal_lock_updates(NEXT3_SB(sb)->s_journal);
		err2 = journal_flush(NEXT3_SB(sb)->s_journal);
		journal_unlock_updates(NEXT3_SB(sb)->s_journal);
		if (err == 0)
			err = err2;
#ifdef CONFIG_NEXT3_FS_SNAPSHOT_CTL
		mutex_unlock(&NEXT3_SB(sb)->s_snapshot_mutex);
#endif
group_add_out:
		mnt_drop_write(filp->f_path.mnt);
		return err;
	}


	default:
		return -ENOTTY;
	}
}

#ifdef CONFIG_COMPAT
long next3_compat_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	/* These are just misnamed, they actually get/put from/to user an int */
	switch (cmd) {
	case NEXT3_IOC32_GETFLAGS:
		cmd = NEXT3_IOC_GETFLAGS;
		break;
	case NEXT3_IOC32_SETFLAGS:
		cmd = NEXT3_IOC_SETFLAGS;
		break;
	case NEXT3_IOC32_GETVERSION:
		cmd = NEXT3_IOC_GETVERSION;
		break;
	case NEXT3_IOC32_SETVERSION:
		cmd = NEXT3_IOC_SETVERSION;
		break;
	case NEXT3_IOC32_GROUP_EXTEND:
		cmd = NEXT3_IOC_GROUP_EXTEND;
		break;
	case NEXT3_IOC32_GETVERSION_OLD:
		cmd = NEXT3_IOC_GETVERSION_OLD;
		break;
	case NEXT3_IOC32_SETVERSION_OLD:
		cmd = NEXT3_IOC_SETVERSION_OLD;
		break;
#ifdef CONFIG_JBD_DEBUG
	case NEXT3_IOC32_WAIT_FOR_READONLY:
		cmd = NEXT3_IOC_WAIT_FOR_READONLY;
		break;
#endif
	case NEXT3_IOC32_GETRSVSZ:
		cmd = NEXT3_IOC_GETRSVSZ;
		break;
	case NEXT3_IOC32_SETRSVSZ:
		cmd = NEXT3_IOC_SETRSVSZ;
		break;
	case NEXT3_IOC_GROUP_ADD:
		break;
	default:
		return -ENOIOCTLCMD;
	}
	return next3_ioctl(file, cmd, (unsigned long) compat_ptr(arg));
}
#endif
