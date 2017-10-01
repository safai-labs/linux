/*
 * Overlayfs NFS export support.
 *
 * Amir Goldstein <amir73il@gmail.com>
 *
 * Copyright (C) 2017 CTERA Networks. All Rights Reserved.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 as published by
 * the Free Software Foundation.
 */

#include <linux/fs.h>
#include <linux/mount.h>
#include <linux/xattr.h>
#include <linux/exportfs.h>
#include "overlayfs.h"
#include "ovl_entry.h"

/* Check if dentry is pure upper ancestry up to connectable root */
static bool ovl_is_pure_upper(struct dentry *dentry, int connectable)
{
	struct dentry *parent = NULL;

	/* For non-connectable non-dir we don't need to check ancestry */
	if (!d_is_dir(dentry) && !connectable)
		return !ovl_dentry_lower(dentry);

	dget(dentry);
	while (!IS_ROOT(dentry) &&
	       !ovl_dentry_lower(dentry)) {
		parent = dget_parent(dentry);
		dput(dentry);
		dentry = parent;
	}
	dput(dentry);

	return dentry == dentry->d_sb->s_root;
}

/* TODO: add export_operations method dentry_to_fh() ??? */
static int ovl_dentry_to_fh(struct dentry *dentry, struct fid *fid,
			    int *max_len, int connectable)
{
	const struct ovl_fh *fh;
	int len = *max_len << 2;

	/* TODO: handle encoding of non pure upper */
	if (!ovl_is_pure_upper(dentry, connectable))
		return FILEID_INVALID;

	fh = ovl_encode_fh(ovl_dentry_upper(dentry), true, connectable);
	if (IS_ERR(fh))
		return FILEID_INVALID;

	if (fh->len > len) {
		kfree(fh);
		return FILEID_INVALID;
	}

	memcpy((char *)fid, (char *)fh, len);
	*max_len = len >> 2;
	kfree(fh);

	return OVL_FILEID_WITHOUT_PARENT;
}

static int ovl_encode_inode_fh(struct inode *inode, u32 *fh, int *max_len,
			       struct inode *parent)
{
	struct dentry *dentry = d_find_alias(inode);
	int type;

	if (!dentry)
		return FILEID_INVALID;

	/*
	 * Parent and child may not be on the same layer.
	 *
	 * TODO: encode connectable file handle as an array of self ovl_fh
	 *       and parent ovl_fh (type OVL_FILEID_WITH_PARENT).
	 */
	if (parent)
		return FILEID_INVALID;

	type = ovl_dentry_to_fh(dentry, (struct fid *)fh, max_len, 0);

	dput(dentry);
	return type;
}

/*
 * Find or instantiate an overlay dentry from real dentries.
 */
static struct dentry *ovl_obtain_alias(struct super_block *sb,
				       struct dentry *upper,
				       struct dentry *lower)
{
	struct inode *inode;
	struct dentry *dentry;
	struct ovl_entry *oe;

	/* TODO: handle decoding of non pure upper */
	if (lower)
		return ERR_PTR(-EINVAL);

	inode = ovl_get_inode(sb, upper, NULL);
	if (IS_ERR(inode))
		return ERR_CAST(inode);

	dentry = d_obtain_alias(inode);
	if (IS_ERR(dentry) || dentry == dentry->d_sb->s_root)
		return dentry;

	if (dentry->d_fsdata) {
		if (WARN_ON(ovl_dentry_lower(dentry) ||
			    ovl_dentry_upper(dentry)->d_inode !=
			    upper->d_inode)) {
			dput(dentry);
			return ERR_PTR(-ESTALE);
		}
		return dentry;
	}

	oe = ovl_alloc_entry(0);
	if (!oe) {
		dput(dentry);
		return ERR_PTR(-ENOMEM);
	}

	oe->has_upper = true;
	dentry->d_fsdata = oe;
	return dentry;

}

static struct dentry *ovl_fh_to_dentry(struct super_block *sb, struct fid *fid,
				       int fh_len, int fh_type)
{
	struct ovl_fs *ofs = sb->s_fs_info;
	struct vfsmount *mnt = ofs->upper_mnt;
	struct dentry *upper;
	struct ovl_fh *fh = (struct ovl_fh *) fid;
	int err;

	if (fh_type != OVL_FILEID_WITHOUT_PARENT)
		return ERR_PTR(-EINVAL);

	err = ovl_check_fh_len(fh, fh_len << 2);
	if (err)
		return ERR_PTR(err);

	/* TODO: handle decoding of non pure upper */
	if (!mnt || !(fh->flags & OVL_FH_FLAG_PATH_UPPER))
		return NULL;

	upper = ovl_decode_fh(fh, mnt);
	if (IS_ERR_OR_NULL(upper))
		return upper;

	/* Find or instantiate a pure upper dentry */
	return ovl_obtain_alias(sb, upper, NULL);
}

static struct dentry *ovl_get_parent(struct dentry *dentry)
{
	const struct export_operations *real_op;
	struct dentry *upper;

	/* TODO: handle connecting of non pure upper */
	if (ovl_dentry_lower(dentry))
		return ERR_PTR(-EACCES);

	upper = ovl_dentry_upper(dentry);
	real_op = upper->d_sb->s_export_op;
	if (!real_op || !real_op->get_parent)
		return ERR_PTR(-EACCES);

	upper = real_op->get_parent(upper);
	if (IS_ERR(upper))
		return upper;

	/* Find or instantiate a pure upper dentry */
	return ovl_obtain_alias(dentry->d_sb, upper, NULL);
}

const struct export_operations ovl_export_operations = {
	.encode_fh      = ovl_encode_inode_fh,
	.fh_to_dentry	= ovl_fh_to_dentry,
	.get_parent	= ovl_get_parent,
};
