/*
 * File: fs/overlayfs/snapshot.c
 *
 * Overlayfs snapshot core functions.
 *
 * Copyright (C) 2016-2017 CTERA Network by Amir Goldstein <amir73il@gmail.com>
 */

#include <linux/fs.h>
#include <linux/mount.h>
#include <linux/namei.h>
#include <linux/xattr.h>
#include "overlayfs.h"
#include "ovl_entry.h"

static int ovl_snapshot_dentry_is_valid(struct dentry *snapdentry,
					struct vfsmount *snapmnt)
{
	/* no snaphsot overlay (pre snapshot take) */
	if (!snapmnt && !snapdentry)
		return 0;

	/* an uninitialized snapdentry after snapshot take */
	if (!snapdentry)
		return -ENOENT;

	/*
	 * snapmnt is NULL and snapdentry is non-NULL
	 * or snapdentry->d_sb != snapmnt->mnt_sb. This implies
	 * a stale snapdentry from an older snapshot overlay
	 */
	if (unlikely(!snapmnt ||
		     snapmnt->mnt_sb != snapdentry->d_sb))
		return -ESTALE;

	return 0;
}

/* return snapshot overlay dentry with elevated refcount */
struct dentry *ovl_snapshot_dentry(struct dentry *dentry)
{
	struct ovl_entry *oe = dentry->d_fsdata;
	struct dentry *snap;

	if (!ovl_is_snapshot_fs_type(dentry->d_sb))
		return NULL;

	rcu_read_lock();
	snap = dget(rcu_dereference(oe->__snapdentry));
	rcu_read_unlock();

	return snap;
}

/* non error return value implies path with elevated refcount */
int ovl_snapshot_path(struct dentry *dentry, struct path *path)
{
	struct ovl_fs *ofs = dentry->d_sb->s_fs_info;
	struct ovl_entry *oe = dentry->d_fsdata;
	struct path snappath;
	int err;

	/* not a snapshot mount */
	if (!ovl_is_snapshot_fs_type(dentry->d_sb))
		return 0;

	rcu_read_lock();
	snappath.mnt = mntget(rcu_dereference(ofs->__snapmnt));
	snappath.dentry = dget(rcu_dereference(oe->__snapdentry));
	rcu_read_unlock();

	err = ovl_snapshot_dentry_is_valid(snappath.dentry, snappath.mnt);
	if (err)
		goto out_err;

	*path = snappath;
	return 0;

out_err:
	path_put(&snappath);
	return err;
}

static int ovl_snapshot_copy_down(struct dentry *dentry)
{
	struct inode *inode = d_inode(dentry);
	struct path snappath = { };
	struct dentry *snap;
	int err;

	err = ovl_snapshot_path(dentry, &snappath);
	if (!err && !snappath.dentry)
		goto out_path_put;

	if (unlikely(err))
		goto bug;

	/*
	 * Snapshot dentry may be positive or negative or point at root.
	 * If positive, it may need to be copied down.
	 * If negative, it may need to be explicitly whited out.
	 * If snapentry is root, but dentry is not, that indicates that
	 * snapentry is nested inside an already whited out directory,
	 * so need to do nothing about it.
	 */
	if (IS_ROOT(snappath.dentry) && !IS_ROOT(dentry))
		goto out_path_put;

	/* Trigger 'copy down' to snapshot */
	snap = d_real(snappath.dentry, NULL, O_RDWR);
	if (unlikely(IS_ERR(snap))) {
		err = PTR_ERR(snap);
		goto bug;
	}

out_path_put:
	path_put(&snappath);
	return 0;
bug:
	WARN(1, "%s(%pd4, %s:%lu): cow failed (err=%d)\n", __func__,
	     dentry, inode ? inode->i_sb->s_id : "NULL",
	     inode ? inode->i_ino : 0, err);

	path_put(&snappath);
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
	if (!ovl_is_snapshot_fs_type(dentry->d_sb))
		return 0;

	return ovl_snapshot_copy_down(dentry);
}

void ovl_snapshot_drop_write(struct dentry *dentry)
{
	struct dentry *snap = ovl_snapshot_dentry(dentry);
	struct inode *inode = d_inode(dentry);

	/*
	 * We may have just dropped this dentry, because it was deleted or
	 * renamed over - then snapshot still thinks it has a lower dentry.
	 * Unhash the snapshot dentry as well in this case.
	 *
	 * Similarly, explicit whiteout in snapshot may have droped the
	 * overlayfs dentry, so if we hold a reference to an unhashed dentry,
	 * drop our dentry.
	 */
	if (snap && !IS_ROOT(snap) &&
	    (d_unhashed(dentry) || d_unhashed(snap))) {
		pr_debug("ovl_snapshot_d_drop(%pd4, %lu): is_dir=%d, negative=%d, unhashed=%d, snap unhashed=%d\n",
			dentry, inode ? inode->i_ino : 0,
			d_is_dir(snap), d_is_negative(dentry),
			d_unhashed(dentry), d_unhashed(snap));
		d_drop(dentry);
		d_drop(snap);
	}
	dput(snap);
}

/*
 * Returns 1 if both snapdentry and snapmnt are NULL or
 * if snapdentry and snapmnt point to the same super block.
 *
 * Returns 0 if snapdentry is NULL and snapmnt is not NULL or
 * if snapdentry and snapmnt point to different super blocks.
 * This will cause vfs lookup to invalidate this dentry and call ovl_lookup()
 * again to re-lookup snapdentry from the current snapmnt.
 */
int ovl_snapshot_revalidate(struct dentry *dentry, unsigned int flags)
{
	struct path snappath = { NULL, NULL };
	int err;

	if (flags & LOOKUP_RCU) {
		struct ovl_fs *ofs = dentry->d_sb->s_fs_info;
		struct ovl_entry *oe = dentry->d_fsdata;

		err = ovl_snapshot_dentry_is_valid(
				rcu_dereference(oe->__snapdentry),
				rcu_dereference(ofs->__snapmnt));
	} else {
		err = ovl_snapshot_path(dentry, &snappath);
		path_put(&snappath);
	}

	if (likely(!err))
		return 1;

	if (err == -ESTALE || err == -ENOENT)
	       return 0;

	return err;
}

void ovl_snapshot_barrier(struct super_block *sb)
{
	struct ovl_fs *ufs = sb->s_fs_info;
	struct ovl_entry *roe = sb->s_root->d_fsdata;
	struct path oldsnappath;

	if (ufs->__snapmnt == ufs->snap_mnt)
		return;

	/*
	 * Make the new snapshot requested in remount effective.
	 * We only update __snapdentry for the snapshot mount root dentry.
	 * All other dentries may still reference the old snapshot overlay.
	 * Those will be invalidated either by shrink_dcache_parent() below
	 * or during RCU lookup by ovl_snapshot_revalidate().
	 * TODO: handle stale dentries of open files.
	 */
	pr_debug("%s: old snap_sid='%s'\n", __func__,
		 ufs->__snapmnt ? ufs->__snapmnt->mnt_sb->s_id : "");
	oldsnappath.mnt = ufs->__snapmnt;
	oldsnappath.dentry = roe->__snapdentry;
	rcu_assign_pointer(ufs->__snapmnt, mntget(ufs->snap_mnt));
	rcu_assign_pointer(roe->__snapdentry, ufs->snap_mnt ?
			   dget(ufs->snap_mnt->mnt_root) : NULL);
	pr_debug("%s: new snap_sid='%s'\n", __func__,
		 ufs->__snapmnt ? ufs->__snapmnt->mnt_sb->s_id : "");

	/* wait grace period before dropping old snapshot overlay */
	synchronize_rcu();
	/* prune invalid dcache entries - this doesn't deal with open files */
	shrink_dcache_parent(sb->s_root);
	/* drop old snapmnt - this may warn about dentries still in use */
	path_put(&oldsnappath);
}
