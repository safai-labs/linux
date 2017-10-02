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
	struct dentry *lower = ovl_dentry_lower(dentry);
	struct dentry *stable;
	const struct ovl_fh *fh;
	int len = *max_len << 2;

	/* TODO: handle encoding of non pure upper */
	if (!ovl_is_pure_upper(dentry, connectable))
		return FILEID_INVALID;

	/*
	 * Choose a stable dentry to export to NFS.
	 * The stable dentry should persist across copy up and should point
	 * to the same inode after mount cycle, so for merge dir and non-dir
	 * with origin, use the origin dentry, otherwise, for pure upper,
	 * use the upper dentry.
	 */
	stable = lower ?: ovl_dentry_upper(dentry);
	if (!stable)
		return FILEID_INVALID;

	/*
	 * When we decode a file handle obtained from a lower dentry, that
	 * lower may have already been copied up. If that lower is not a
	 * hardlink, then it may not be indexed on copy up. In that case,
	 * the way we get to the upper inode when decoding is by looking up
	 * the overlay using the same path as lower dentry, but to do that,
	 * we need a connected lower dentry, so we encode a conneccted lower
	 * file handle even if we were asked for a non-connecctable file
	 * handle. The problem with connectable file handles is that they
	 * may not be unique, because they contain the parent inode encoding
	 * and dentry can change its parent. We rely on the assumption that
	 * lower layer is not expected to be changed and therefore the
	 * connectable lower file handle is "likely unique". If lower layer
	 * is changed, then the new encoded file handle will apear to nfsd
	 * as a new file and the old file handle may become stale.
	 * We cannot assume uniqeness of connectable file handle for a lower
	 * hardlink, so instead, we copy up the lower hardlink before encoding
	 * the file handle, so we can use the index on decode.
	 */
	if (lower && !d_is_dir(lower)) {
		if (d_inode(lower)->i_nlink == 1) {
			connectable = 1;
		} else if (stable == lower) {
			int err;

			if (ovl_want_write(dentry))
				return FILEID_INVALID;

			err = ovl_copy_up(dentry);

			ovl_drop_write(dentry);
			if (err)
				return FILEID_INVALID;
		}
	}

	fh = ovl_encode_fh(stable, !lower, connectable);
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

	/*
	 * This may be a diconnected dentry. We must not allow it to get passed
	 * ovl_dentry_has_upper_alias() check in ovl_copy_up().
	 */
	oe->has_upper = true;
	dentry->d_fsdata = oe;
	return dentry;

}

static struct dentry *ovl_fh_to_dentry(struct super_block *sb, struct fid *fid,
				       int fh_len, int fh_type)
{
	struct ovl_fs *ofs = sb->s_fs_info;
	struct dentry *upper;
	struct dentry *index = NULL;
	struct dentry *origin = NULL;
	struct ovl_fh *fh = (struct ovl_fh *) fid;
	int err, i;

	if (fh_type != OVL_FILEID_WITHOUT_PARENT)
		return ERR_PTR(-EINVAL);

	err = ovl_check_fh_len(fh, fh_len << 2);
	if (err)
		return ERR_PTR(err);

	if (fh->flags & OVL_FH_FLAG_PATH_UPPER) {
		if (!ofs->upper_mnt)
			return NULL;

		upper = ovl_decode_fh(fh, ofs->upper_mnt);
		if (IS_ERR_OR_NULL(upper))
			return upper;

		/* Find or instantiate a pure upper dentry */
		return ovl_obtain_alias(sb, upper, NULL);
	}

	/* Find lower layer by UUID and decode */
	for (i = 0; i < ofs->numlower; i++) {
		origin = ovl_decode_fh(fh, ofs->lower_mnt[i]);
		if (origin)
			break;
	}

	if (IS_ERR_OR_NULL(origin))
		return origin;

	/* TODO: check if index exists to instantiate overlay dentry */
	if (index)
		return ovl_obtain_alias(sb, index, origin);

	if (origin->d_flags & DCACHE_DISCONNECTED) {
		/* With no lower path and no index we are lost */
		dput(origin);
		return NULL;
	}

	/*
	 * TODO: walk back parent chain to lower mnt root and check if
	 * parents are indexed. When reaching indexed or root, lookup
	 * relative path from upper dir, while instantiating and connecting
	 * the overlay dentries on the way to the dentry we are decoding.
	 */
	return NULL;
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
