/*
 * File: fs/overlayfs/snapshot.c
 *
 * Overlayfs snapshot core functions.
 *
 * Copyright (C) 2016-2017 CTERA Network by Amir Goldstein <amir73il@gmail.com>
 */

#include <linux/fs.h>
#include <linux/xattr.h>
#include "overlayfs.h"
#include "ovl_entry.h"

struct dentry *ovl_snapshot_dentry(struct dentry *dentry)
{
	struct ovl_entry *oe = dentry->d_fsdata;

	if (!ovl_is_snapshot_fs_type(dentry->d_sb))
		return NULL;

	return oe->__snapdentry;
}

static int ovl_snapshot_copy_down(struct dentry *dentry)
{
	struct inode *inode = d_inode(dentry);
	struct dentry *snap;

	snap = ovl_snapshot_dentry(dentry);
	/*
	 * Snapshot dentry may be positive or negative or NULL.
	 * If positive, it may need to be copied down.
	 * If negative, it may need to be explicitly whited out.
	 * Otherwise, the entry is nested inside an already
	 * whited out directory, so need to do nothing about it.
	 */
	if (!snap)
		return 0;

	/* Trigger 'copy down' to snapshot */
	snap = d_real(snap, NULL, O_RDWR);
	if (IS_ERR(snap))
		goto bug;

	return 0;
bug:
	WARN(1, "%s(%pd4, %s:%lu): cow failed (err=%ld)\n", __func__,
	     dentry, inode ? inode->i_sb->s_id : "NULL",
	     inode ? inode->i_ino : 0, PTR_ERR(snap));

	/* Allowing write would corrupt snapshot so deny */
	return -EROFS;
}

struct dentry *ovl_snapshot_d_real(struct dentry *dentry,
				   const struct inode *inode,
				   unsigned int open_flags)
{
	struct dentry *real;

	if (d_is_dir(dentry)) {
		if (!inode || inode == d_inode(dentry))
			return dentry;
		goto bug;
	}

	if (d_is_negative(dentry))
		return dentry;

	if (open_flags & (O_ACCMODE|O_TRUNC)) {
		int err = ovl_snapshot_copy_down(dentry);

		if (err)
			return ERR_PTR(err);
	}

	/* With snapshot, the real inode is always the upper */
	real = ovl_dentry_upper(dentry);
	if (!real)
		goto bug;

	if (!inode || inode == d_inode(real))
		return real;

bug:
	WARN(1, "%s(%pd4, %s:%lu): real dentry not found\n", __func__,
	     dentry, inode ? inode->i_sb->s_id : "NULL",
	     inode ? inode->i_ino : 0);
	return dentry;
}

int ovl_snapshot_want_write(struct dentry *dentry)
{
	struct ovl_fs *ofs = dentry->d_sb->s_fs_info;

	if (!ofs->snapshot_mnt)
		return 0;

	return ovl_snapshot_copy_down(dentry);
}

void ovl_snapshot_drop_write(struct dentry *dentry)
{
}
