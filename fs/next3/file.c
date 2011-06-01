/*
 *  linux/fs/next3/file.c
 *
 * Copyright (C) 1992, 1993, 1994, 1995
 * Remy Card (card@masi.ibp.fr)
 * Laboratoire MASI - Institut Blaise Pascal
 * Universite Pierre et Marie Curie (Paris VI)
 *
 *  from
 *
 *  linux/fs/minix/file.c
 *
 *  Copyright (C) 1991, 1992  Linus Torvalds
 *
 *  next3 fs regular file handling primitives
 *
 *  64-bit file support on 64-bit platforms by Jakub Jelinek
 *	(jj@sunsite.ms.mff.cuni.cz)
 *
 *  Added snapshot support, Amir Goldstein <amir73il@users.sf.net>, 2008
 */

#include <linux/time.h>
#include <linux/fs.h>
#include <linux/jbd.h>
#include <linux/quotaops.h>
#include "next3.h"
#include "next3_jbd.h"
#include "xattr.h"
#include "acl.h"
#include "snapshot.h"

#ifdef CONFIG_NEXT3_FS_SNAPSHOT_FILE_PERM
static int next3_file_open(struct inode *inode, struct file *filp)
{
	if (next3_snapshot_file(inode) &&
		(filp->f_flags & O_ACCMODE) != O_RDONLY)
		/*
		 * allow only read-only access to snapshot files
		 */
		return -EPERM;

	return dquot_file_open(inode, filp);
}

#endif
/*
 * Called when an inode is released. Note that this is different
 * from next3_file_open: open gets called at every open, but release
 * gets called only when /all/ the files are closed.
 */
static int next3_release_file (struct inode * inode, struct file * filp)
{
	if (next3_test_inode_state(inode, NEXT3_STATE_FLUSH_ON_CLOSE)) {
		filemap_flush(inode->i_mapping);
		next3_clear_inode_state(inode, NEXT3_STATE_FLUSH_ON_CLOSE);
	}
	/* if we are the last writer on the inode, drop the block reservation */
	if ((filp->f_mode & FMODE_WRITE) &&
			(atomic_read(&inode->i_writecount) == 1))
	{
		mutex_lock(&NEXT3_I(inode)->truncate_mutex);
		next3_discard_reservation(inode);
		mutex_unlock(&NEXT3_I(inode)->truncate_mutex);
	}
	if (is_dx(inode) && filp->private_data)
		next3_htree_free_dir_info(filp->private_data);

	return 0;
}

#ifdef CONFIG_NEXT3_FS_SNAPSHOT_FILE_HUGE
/*
 * next3_llseek() copied from generic_file_llseek() to handle both
 * snapshot and non-snapshot files maxbytes values. This should
 * otherwise be identical with generic_file_llseek().
 */
loff_t next3_llseek(struct file *file, loff_t offset, int origin)
{
	struct inode *inode = file->f_mapping->host;
	loff_t maxbytes;

	if (!next3_snapshot_file(inode))
		maxbytes = NEXT3_SB(inode->i_sb)->s_bitmap_maxbytes;
	else
		maxbytes = inode->i_sb->s_maxbytes;
	mutex_lock(&inode->i_mutex);
	switch (origin) {
	case SEEK_END:
		offset += inode->i_size;
		break;
	case SEEK_CUR:
		if (offset == 0) {
			mutex_unlock(&inode->i_mutex);
			return file->f_pos;
		}
		offset += file->f_pos;
		break;
	}

	if (offset < 0 || offset > maxbytes) {
		mutex_unlock(&inode->i_mutex);
		return -EINVAL;
	}

	if (offset != file->f_pos) {
		file->f_pos = offset;
		file->f_version = 0;
	}
	mutex_unlock(&inode->i_mutex);

	return offset;
}

static ssize_t
next3_file_write(struct kiocb *iocb, const struct iovec *iov,
		unsigned long nr_segs, loff_t pos)
{
	struct inode *inode = iocb->ki_filp->f_path.dentry->d_inode;
	int ret;

	/*
	 * Snapshot files are read-only and for non-snapshot file,
	 * the size limit is smaller than s_maxbytes.
	 */

	if (!next3_snapshot_file(inode)) {
		struct next3_sb_info *sbi = NEXT3_SB(inode->i_sb);
		size_t length = iov_length(iov, nr_segs);

		if ((pos > sbi->s_bitmap_maxbytes ||
		    (pos == sbi->s_bitmap_maxbytes && length > 0)))
			return -EFBIG;

		if (pos + length > sbi->s_bitmap_maxbytes) {
			nr_segs = iov_shorten((struct iovec *)iov, nr_segs,
					      sbi->s_bitmap_maxbytes - pos);
		}
	}

	ret = generic_file_aio_write(iocb, iov, nr_segs, pos);

	return ret;
}

#endif
const struct file_operations next3_file_operations = {
#ifdef CONFIG_NEXT3_FS_SNAPSHOT_FILE_HUGE
	.llseek		= next3_llseek,
#else
	.llseek		= generic_file_llseek,
#endif
	.read		= do_sync_read,
	.write		= do_sync_write,
	.aio_read	= generic_file_aio_read,
#ifdef CONFIG_NEXT3_FS_SNAPSHOT_FILE_HUGE
	.aio_write	= next3_file_write,
#else
	.aio_write	= generic_file_aio_write,
#endif
	.unlocked_ioctl	= next3_ioctl,
#ifdef CONFIG_COMPAT
	.compat_ioctl	= next3_compat_ioctl,
#endif
	.mmap		= generic_file_mmap,
#ifdef CONFIG_NEXT3_FS_SNAPSHOT_FILE_PERM
	.open		= next3_file_open,
#else
	.open		= dquot_file_open,
#endif
	.release	= next3_release_file,
	.fsync		= next3_sync_file,
	.splice_read	= generic_file_splice_read,
	.splice_write	= generic_file_splice_write,
};

const struct inode_operations next3_file_inode_operations = {
	.truncate	= next3_truncate,
	.setattr	= next3_setattr,
#ifdef CONFIG_NEXT3_FS_XATTR
	.setxattr	= generic_setxattr,
	.getxattr	= generic_getxattr,
	.listxattr	= next3_listxattr,
	.removexattr	= generic_removexattr,
#endif
	.check_acl	= next3_check_acl,
	.fiemap		= next3_fiemap,
};

