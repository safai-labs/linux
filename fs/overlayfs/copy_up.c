/*
 *
 * Copyright (C) 2011 Novell Inc.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 as published by
 * the Free Software Foundation.
 */

#include <linux/module.h>
#include <linux/fs.h>
#include <linux/slab.h>
#include <linux/file.h>
#include <linux/splice.h>
#include <linux/xattr.h>
#include <linux/security.h>
#include <linux/uaccess.h>
#include <linux/sched/signal.h>
#include <linux/cred.h>
#include <linux/namei.h>
#include <linux/fdtable.h>
#include <linux/ratelimit.h>
#include <linux/exportfs.h>
#include "overlayfs.h"
#include "ovl_entry.h"

#define OVL_COPY_UP_CHUNK_SIZE (1 << 20)

static bool __read_mostly ovl_check_copy_up;
module_param_named(check_copy_up, ovl_check_copy_up, bool,
		   S_IWUSR | S_IRUGO);
MODULE_PARM_DESC(ovl_check_copy_up,
		 "Warn on copy-up when causing process also has a R/O fd open");

static int ovl_check_fd(const void *data, struct file *f, unsigned int fd)
{
	const struct dentry *dentry = data;

	if (file_inode(f) == d_inode(dentry))
		pr_warn_ratelimited("overlayfs: Warning: Copying up %pD, but open R/O on fd %u which will cease to be coherent [pid=%d %s]\n",
				    f, fd, current->pid, current->comm);
	return 0;
}

/*
 * Check the fds open by this process and warn if something like the following
 * scenario is about to occur:
 *
 *	fd1 = open("foo", O_RDONLY);
 *	fd2 = open("foo", O_RDWR);
 */
static void ovl_do_check_copy_up(struct dentry *dentry)
{
	if (ovl_check_copy_up)
		iterate_fd(current->files, 0, ovl_check_fd, dentry);
}

int ovl_copy_xattr(struct dentry *old, struct dentry *new)
{
	ssize_t list_size, size, value_size = 0;
	char *buf, *name, *value = NULL;
	int uninitialized_var(error);
	size_t slen;

	if (!(old->d_inode->i_opflags & IOP_XATTR) ||
	    !(new->d_inode->i_opflags & IOP_XATTR))
		return 0;

	list_size = vfs_listxattr(old, NULL, 0);
	if (list_size <= 0) {
		if (list_size == -EOPNOTSUPP)
			return 0;
		return list_size;
	}

	buf = kzalloc(list_size, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;

	list_size = vfs_listxattr(old, buf, list_size);
	if (list_size <= 0) {
		error = list_size;
		goto out;
	}

	for (name = buf; list_size; name += slen) {
		slen = strnlen(name, list_size) + 1;

		/* underlying fs providing us with an broken xattr list? */
		if (WARN_ON(slen > list_size)) {
			error = -EIO;
			break;
		}
		list_size -= slen;

		if (ovl_is_private_xattr(name))
			continue;
retry:
		size = vfs_getxattr(old, name, value, value_size);
		if (size == -ERANGE)
			size = vfs_getxattr(old, name, NULL, 0);

		if (size < 0) {
			error = size;
			break;
		}

		if (size > value_size) {
			void *new;

			new = krealloc(value, size, GFP_KERNEL);
			if (!new) {
				error = -ENOMEM;
				break;
			}
			value = new;
			value_size = size;
			goto retry;
		}

		error = security_inode_copy_up_xattr(name);
		if (error < 0 && error != -EOPNOTSUPP)
			break;
		if (error == 1) {
			error = 0;
			continue; /* Discard */
		}
		error = vfs_setxattr(new, name, value, size, 0);
		if (error)
			break;
	}
	kfree(value);
out:
	kfree(buf);
	return error;
}

static int ovl_copy_up_data(struct path *old, struct path *new, loff_t len)
{
	struct file *old_file;
	struct file *new_file;
	loff_t old_pos = 0;
	loff_t new_pos = 0;
	int error = 0;

	if (len == 0)
		return 0;

	old_file = ovl_path_open(old, O_LARGEFILE | O_RDONLY);
	if (IS_ERR(old_file))
		return PTR_ERR(old_file);

	new_file = ovl_path_open(new, O_LARGEFILE | O_WRONLY);
	if (IS_ERR(new_file)) {
		error = PTR_ERR(new_file);
		goto out_fput;
	}

	/* Try to use clone_file_range to clone up within the same fs */
	error = vfs_clone_file_range(old_file, 0, new_file, 0, len);
	if (!error)
		goto out;
	/* Couldn't clone, so now we try to copy the data */
	error = 0;

	/* FIXME: copy up sparse files efficiently */
	while (len) {
		size_t this_len = OVL_COPY_UP_CHUNK_SIZE;
		long bytes;

		if (len < this_len)
			this_len = len;

		if (signal_pending_state(TASK_KILLABLE, current)) {
			error = -EINTR;
			break;
		}

		bytes = do_splice_direct(old_file, &old_pos,
					 new_file, &new_pos,
					 this_len, SPLICE_F_MOVE);
		if (bytes <= 0) {
			error = bytes;
			break;
		}
		WARN_ON(old_pos != new_pos);

		len -= bytes;
	}
out:
	if (!error)
		error = vfs_fsync(new_file, 0);
	fput(new_file);
out_fput:
	fput(old_file);
	return error;
}

static int ovl_set_timestamps(struct dentry *upperdentry, struct kstat *stat)
{
	struct iattr attr = {
		.ia_valid =
		     ATTR_ATIME | ATTR_MTIME | ATTR_ATIME_SET | ATTR_MTIME_SET,
		.ia_atime = stat->atime,
		.ia_mtime = stat->mtime,
	};

	return notify_change(upperdentry, &attr, NULL);
}

int ovl_set_attr(struct dentry *upperdentry, struct kstat *stat)
{
	int err = 0;

	if (!S_ISLNK(stat->mode)) {
		struct iattr attr = {
			.ia_valid = ATTR_MODE,
			.ia_mode = stat->mode,
		};
		err = notify_change(upperdentry, &attr, NULL);
	}
	if (!err) {
		struct iattr attr = {
			.ia_valid = ATTR_UID | ATTR_GID,
			.ia_uid = stat->uid,
			.ia_gid = stat->gid,
		};
		err = notify_change(upperdentry, &attr, NULL);
	}
	if (!err)
		ovl_set_timestamps(upperdentry, stat);

	return err;
}

struct ovl_fh *ovl_encode_fh(struct dentry *lower, bool is_upper)
{
	struct ovl_fh *fh;
	int fh_type, fh_len, dwords;
	void *buf;
	int buflen = MAX_HANDLE_SZ;
	uuid_be *uuid = (uuid_be *) &lower->d_sb->s_uuid;

	buf = kmalloc(buflen, GFP_TEMPORARY);
	if (!buf)
		return ERR_PTR(-ENOMEM);

	/*
	 * We encode a non-connectable file handle for non-dir, because we
	 * only need to find the lower inode number and we don't want to pay
	 * the price or reconnecting the dentry.
	 */
	dwords = buflen >> 2;
	fh_type = exportfs_encode_fh(lower, buf, &dwords, 0);
	buflen = (dwords << 2);

	fh = ERR_PTR(-EIO);
	if (WARN_ON(fh_type < 0) ||
	    WARN_ON(buflen > MAX_HANDLE_SZ) ||
	    WARN_ON(fh_type == FILEID_INVALID))
		goto out;

	BUILD_BUG_ON(MAX_HANDLE_SZ + offsetof(struct ovl_fh, fid) > 255);
	fh_len = offsetof(struct ovl_fh, fid) + buflen;
	fh = kmalloc(fh_len, GFP_KERNEL);
	if (!fh) {
		fh = ERR_PTR(-ENOMEM);
		goto out;
	}

	fh->version = OVL_FH_VERSION;
	fh->magic = OVL_FH_MAGIC;
	fh->type = fh_type;
	fh->flags = OVL_FH_FLAG_CPU_ENDIAN;
	/*
	 * When we will want to decode an overlay dentry from this handle
	 * and all layers are on the same fs, if we get a disconncted real
	 * dentry when we decode fid, the only way to tell if we should assign
	 * it to upperdentry or to lowerstack is by checking this flag.
	 */
	if (is_upper)
		fh->flags |= OVL_FH_FLAG_PATH_UPPER;
	fh->len = fh_len;
	fh->uuid = *uuid;
	memcpy(fh->fid, buf, buflen);

out:
	kfree(buf);
	return fh;
}

static int ovl_set_origin(struct dentry *dentry, struct dentry *lower,
			  struct dentry *upper)
{
	const struct ovl_fh *fh = NULL;
	int err;

	/*
	 * When lower layer doesn't support export operations store a 'null' fh,
	 * so we can use the overlay.origin xattr to distignuish between a copy
	 * up and a pure upper inode.
	 */
	if (ovl_can_decode_fh(lower->d_sb)) {
		fh = ovl_encode_fh(lower, false);
		if (IS_ERR(fh))
			return PTR_ERR(fh);
	}

	/*
	 * Do not fail when upper doesn't support xattrs.
	 */
	err = ovl_check_setxattr(dentry, upper, OVL_XATTR_ORIGIN, fh,
				 fh ? fh->len : 0, 0);
	kfree(fh);

	return err;
}

static int ovl_copy_up_inode(struct dentry *dentry, struct dentry *temp,
			     struct path *lowerpath, struct kstat *stat)
{
	int err;

	if (S_ISREG(stat->mode)) {
		struct path upperpath;

		ovl_path_upper(dentry, &upperpath);
		BUG_ON(upperpath.dentry != NULL);
		upperpath.dentry = temp;

		err = ovl_copy_up_data(lowerpath, &upperpath, stat->size);
		if (err)
			return err;
	}

	err = ovl_copy_xattr(lowerpath->dentry, temp);
	if (err)
		return err;

	inode_lock(temp->d_inode);
	err = ovl_set_attr(temp, stat);
	inode_unlock(temp->d_inode);
	if (err)
		return err;

	/*
	 * Store identifier of lower inode in upper inode xattr to
	 * allow lookup of the copy up origin inode. We do this last
	 * to bless the inode, in case it was created in the index dir.
	 */
	err = ovl_set_origin(dentry, lowerpath->dentry, temp);

	return err;
}

/*
 * Context and operations for copying up a single lower file.
 */
struct ovl_copy_up_ctx {
	struct dentry *dentry;
	struct path *lowerpath;
	struct kstat *stat;
	struct kstat pstat;
	const char *link;
	struct dentry *upperdir;
	struct dentry *tempdir;
	struct dentry *upper;
	struct dentry *temp;
	bool created;
};

struct ovl_copy_up_ops {
	int (*aquire)(struct ovl_copy_up_ctx *);
	int (*prepare)(struct ovl_copy_up_ctx *);
	int (*commit)(struct ovl_copy_up_ctx *);
	void (*cancel)(struct ovl_copy_up_ctx *);
	void (*release)(struct ovl_copy_up_ctx *);
};

/*
 * Copy up operations using workdir.
 * Upper file is created in workdir, copied and moved to upperdir.
 */
static int ovl_copy_up_workdir_aquire(struct ovl_copy_up_ctx *ctx)
{
	int err = -EIO;

	if (lock_rename(ctx->tempdir, ctx->upperdir) != NULL) {
		pr_err("overlayfs: failed to lock workdir+upperdir\n");
		goto out_unlock;
	}
	if (ovl_dentry_upper(ctx->dentry)) {
		/* Raced with another copy-up? */
		err = 1;
		goto out_unlock;
	}

	return 0;

out_unlock:
	unlock_rename(ctx->tempdir, ctx->upperdir);
	return err;
}

static int ovl_copy_up_workdir_prepare(struct ovl_copy_up_ctx *ctx)
{
	struct dentry *upper = NULL;
	struct dentry *temp = NULL;
	int err;
	struct cattr cattr = {
		/* Can't properly set mode on creation because of the umask */
		.mode = ctx->stat->mode & S_IFMT,
		.rdev = ctx->stat->rdev,
		.link = ctx->link,
	};

	upper = lookup_one_len(ctx->dentry->d_name.name, ctx->upperdir,
			       ctx->dentry->d_name.len);
	err = PTR_ERR(upper);
	if (IS_ERR(upper))
		goto out;

	temp = ovl_lookup_temp(ctx->tempdir);
	if (IS_ERR(temp)) {
		err = PTR_ERR(temp);
		goto out_dput_upper;
	}

	err = ovl_create_real(d_inode(ctx->tempdir), temp, &cattr, NULL, true);
	if (err)
		goto out_dput_temp;

	ctx->upper = upper;
	ctx->temp = temp;
	return 0;

out_dput_temp:
	dput(temp);
out_dput_upper:
	dput(upper);
out:
	return err;
}

static int ovl_copy_up_workdir_commit(struct ovl_copy_up_ctx *ctx)
{
	int err;

	err = ovl_do_rename(d_inode(ctx->tempdir), ctx->temp,
			    d_inode(ctx->upperdir), ctx->upper, 0);
	if (err)
		return err;

	/* After rename, ctx->temp is the upper entry we will use */
	swap(ctx->temp, ctx->upper);

	/* Restore timestamps on parent (best effort) */
	ovl_set_timestamps(ctx->upperdir, &ctx->pstat);

	return err;
}

static void ovl_copy_up_workdir_cancel(struct ovl_copy_up_ctx *ctx)
{
	ovl_cleanup(d_inode(ctx->tempdir), ctx->temp);
}

static void ovl_copy_up_workdir_release(struct ovl_copy_up_ctx *ctx)
{
	unlock_rename(ctx->tempdir, ctx->upperdir);
}

static const struct ovl_copy_up_ops ovl_copy_up_workdir_ops = {
	.aquire = ovl_copy_up_workdir_aquire,
	.prepare = ovl_copy_up_workdir_prepare,
	.commit = ovl_copy_up_workdir_commit,
	.cancel = ovl_copy_up_workdir_cancel,
	.release = ovl_copy_up_workdir_release,
};

/*
 * Copy up operations using O_TMPFILE.
 * Upper file is created unlinked, copied and linked to upperdir.
 */
static int ovl_copy_up_tmpfile_aquire(struct ovl_copy_up_ctx *ctx)
{
	return ovl_copy_up_start(ctx->dentry);
}

static int ovl_copy_up_tmpfile_prepare(struct ovl_copy_up_ctx *ctx)
{
	struct dentry *upper;
	struct dentry *temp;

	upper = lookup_one_len_unlocked(ctx->dentry->d_name.name, ctx->upperdir,
					ctx->dentry->d_name.len);
	if (IS_ERR(upper))
		return PTR_ERR(upper);

	temp = ovl_do_tmpfile(ctx->upperdir, ctx->stat->mode);
	if (IS_ERR(temp)) {
		dput(upper);
		return PTR_ERR(temp);
	}

	ctx->upper = upper;
	ctx->temp = temp;
	return 0;
}

static int ovl_copy_up_tmpfile_commit(struct ovl_copy_up_ctx *ctx)
{
	int err;

	inode_lock_nested(d_inode(ctx->upperdir), I_MUTEX_PARENT);
	/* link the sucker ;) */
	err = ovl_do_link(ctx->temp, d_inode(ctx->upperdir), ctx->upper, true);
	/* Restore timestamps on parent (best effort) */
	if (!err)
		ovl_set_timestamps(ctx->upperdir, &ctx->pstat);
	inode_unlock(d_inode(ctx->upperdir));

	return err;
}

static void ovl_copy_up_tmpfile_cancel(struct ovl_copy_up_ctx *ctx) { }

static void ovl_copy_up_tmpfile_release(struct ovl_copy_up_ctx *ctx)
{
	ovl_copy_up_end(ctx->dentry);
}

static const struct ovl_copy_up_ops ovl_copy_up_tmpfile_ops = {
	.aquire = ovl_copy_up_tmpfile_aquire,
	.prepare = ovl_copy_up_tmpfile_prepare,
	.commit = ovl_copy_up_tmpfile_commit,
	.cancel = ovl_copy_up_tmpfile_cancel,
	.release = ovl_copy_up_tmpfile_release,
};

/*
 * Copy up operations using index dir.
 * Upper file is created in index dir, copied and linked to upperdir.
 * The index entry remains to be used for mapping lower inode to upper inode.
 */
static int ovl_copy_up_indexdir_aquire(struct ovl_copy_up_ctx *ctx)
{
	return ovl_copy_up_start(ctx->dentry);
}

/*
 * Set ctx->temp to a positive dentry with the index inode.
 *
 * Return 0 if entry was created by us and we need to copy the inode data.
 *
 * Return 1 if we found an index inode, in which case, we do not need
 * to copy the inode data.
 *
 * May return -EEXISTS/-ENOENT if corrupt index entries are found.
 */
static int ovl_copy_up_indexdir_prepare(struct ovl_copy_up_ctx *ctx)
{
	struct dentry *upper = NULL;
	struct dentry *index = NULL;
	struct inode *inode;
	int err;
	struct cattr cattr = {
		/* Can't properly set mode on creation because of the umask */
		.mode = ctx->stat->mode & S_IFMT,
		.rdev = ctx->stat->rdev,
		.link = ctx->link,
	};

	if (ctx->upperdir) {
		upper = lookup_one_len_unlocked(ctx->dentry->d_name.name,
						ctx->upperdir,
						ctx->dentry->d_name.len);
		if (IS_ERR(upper))
			return PTR_ERR(upper);
	}

	err = ovl_lookup_index(ctx->dentry, NULL, ctx->lowerpath->dentry,
			       &index);
	if (err)
		goto out_dput;

	inode = d_inode(index);
	if (inode) {
		/* Another lower hardlink already copied-up? */
		err = -EEXIST;
		if ((inode->i_mode & S_IFMT) != cattr.mode)
			goto out_dput;

		err = -ENOENT;
		if (!inode->i_nlink)
			goto out_dput;

		/*
		 * Verify that found index is a copy up of lower inode.
		 * If index inode doesn't point back to lower inode via
		 * origin file handle, then this is either a leftover from
		 * failed copy up or an index dir entry before copying layers.
		 * In both cases, we cannot use this index and must fail the
		 * copy up. The failed copy up case will return -EEXISTS and
		 * the copying layers case will return -ESTALE.
		 */
		err = ovl_verify_origin(index, ctx->lowerpath->mnt,
					ctx->lowerpath->dentry, false, false);
		if (err) {
			if (err == -ENODATA)
				err = -EEXIST;
			goto out_dput;
		}

		err = -ENOENT;
		if (ctx->dentry->d_inode->i_nlink == 0) {
			/*
			 * An orphan index inode can be created by copying up
			 * all lower hardlinks and then unlinking all upper
			 * hardlinks. The overlay inode may still be alive if
			 * it is referenced from an open file descriptor, but
			 * there should be no more copy ups that link to the
			 * index inode.
			 * Orphan index inodes should be cleaned up on mount
			 * and when overlay inode nlink drops to zero.
			 */
			pr_warn_ratelimited("overlayfs: link-up orphan index (%pd2, ino=%lu, nlink=%u)\n",
					    index, inode->i_ino,
					    inode->i_nlink);

			if (inode->i_nlink == 1)
				goto out_dput;

			/*
			 * If index is has nlink > 1, then we either have a bug
			 * with persistent union nlink or a lower hardlink was
			 * added while overlay is mounted. Adding a lower
			 * hardlink and then unlinking all overlay hardlinks
			 * would drop overlay nlink to zero before all upper
			 * inodes are unlinked. As a safety measure, when that
			 * situation is detected, set the overlay nlink to the
			 * index inode nlink minus one for the index entry.
			 */
			set_nlink(d_inode(ctx->dentry), inode->i_nlink - 1);
		}

		/* Link to existing upper without copying lower */
		err = 1;
		goto out;
	}

	inode_lock_nested(d_inode(ctx->tempdir), I_MUTEX_PARENT);
	err = ovl_create_real(d_inode(ctx->tempdir), index, &cattr, NULL, true);
	inode_unlock(d_inode(ctx->tempdir));
	if (err)
		goto out_dput;

	ctx->created = true;
out:
	/*
	 * The overlay inode nlink does not change on copy up whether the
	 * operation succeeds or fails, but the upper inode nlink may change.
	 * Therefore, before copy up, we store the union nlink value relative
	 * to the lower inode nlink in an index inode xattr. We will store it
	 * again relative to index inode nlink at copy up commit or cancel.
	 */
	if (ctx->stat->nlink)
		ovl_set_nlink(d_inode(ctx->dentry), index, false);

	ctx->upper = upper;
	ctx->temp = index;
	return err;

out_dput:
	pr_warn_ratelimited("overlayfs: failed to create index entry (%pd2, ino=%lu, err=%i)\n",
			    index ? : ctx->lowerpath->dentry,
			    inode ? inode->i_ino : 0, err);
	pr_warn_ratelimited("overlayfs: try clearing index dir or mounting with '-o index=off' to disable inodes index.\n");
	dput(upper);
	dput(index);
	return err;
}

static int ovl_fsync_index(struct dentry *dentry, struct dentry *index)
{
	struct file *file;
	struct path upperpath;
	int err;

	ovl_path_upper(dentry, &upperpath);
	BUG_ON(upperpath.dentry != NULL);
	upperpath.dentry = index;

	file = ovl_path_open(&upperpath, O_LARGEFILE | O_WRONLY);
	if (IS_ERR(file))
		return PTR_ERR(file);

	err = vfs_fsync(file, 0);

	fput(file);
	return err;
}

static int ovl_copy_up_indexdir_commit(struct ovl_copy_up_ctx *ctx)
{
	int err;

	/*
	 * fsync index before "link-up" to guaranty that nlink xattr is stored
	 * on-disk. Non empty regular files are fsynced on ovl_copy_up_data().
	 * This does not cover "link-up" of non-regular files.
	 *
	 * XXX: Is this really needed? I think that on a journalled file system
	 * inode xattr change cannot be re-ordered with the same inode's nlink
	 * change, because they are both metadata changes of that inode.
	 */
	if ((!ctx->created || !ctx->stat->size) && S_ISREG(ctx->stat->mode)) {
		err = ovl_fsync_index(ctx->dentry, ctx->temp);
		if (err)
			goto out;
	}

	inode_lock_nested(d_inode(ctx->upperdir), I_MUTEX_PARENT);
	/* link the sucker ;) */
	err = ovl_do_link(ctx->temp, d_inode(ctx->upperdir), ctx->upper, true);
	/* Restore timestamps on parent (best effort) */
	if (!err)
		ovl_set_timestamps(ctx->upperdir, &ctx->pstat);
	inode_unlock(d_inode(ctx->upperdir));

	if (err)
		goto out;

	/* Store the union nlink value relative to index inode nlink */
	ovl_set_nlink(d_inode(ctx->dentry), ctx->temp, true);

	/* We can mark dentry is indexed before updating upperdentry */
	ovl_dentry_set_indexed(ctx->dentry);

out:
	return err;
}

static void ovl_copy_up_indexdir_cancel(struct ovl_copy_up_ctx *ctx)
{
	struct inode *inode = d_inode(ctx->temp);

	if (WARN_ON(!inode))
		return;

	/* Store the union nlink value relative to index inode nlink */
	if (ctx->stat->nlink)
		ovl_set_nlink(d_inode(ctx->dentry), ctx->temp, true);

	/* Cleanup prepared index entry only if we created it */
	if (!ctx->created)
		return;

	pr_warn_ratelimited("overlayfs: cleanup bad index (%pd2, ino=%lu, nlink=%u)\n",
			    ctx->temp, inode->i_ino, inode->i_nlink);

	inode_lock_nested(d_inode(ctx->tempdir), I_MUTEX_PARENT);
	ovl_cleanup(d_inode(ctx->tempdir), ctx->temp);
	inode_unlock(d_inode(ctx->tempdir));
}

static void ovl_copy_up_indexdir_release(struct ovl_copy_up_ctx *ctx)
{
	ovl_copy_up_end(ctx->dentry);
}

static const struct ovl_copy_up_ops ovl_copy_up_indexdir_ops = {
	.aquire = ovl_copy_up_indexdir_aquire,
	.prepare = ovl_copy_up_indexdir_prepare,
	.commit = ovl_copy_up_indexdir_commit,
	.cancel = ovl_copy_up_indexdir_cancel,
	.release = ovl_copy_up_indexdir_release,
};

static int ovl_copy_up_locked(struct ovl_copy_up_ctx *ctx,
			      const struct ovl_copy_up_ops *ops)
{
	struct dentry *dentry = ctx->dentry;
	struct dentry *newdentry;
	const struct cred *old_creds = NULL;
	struct cred *new_creds = NULL;
	int err;

	err = security_inode_copy_up(dentry, &new_creds);
	if (err < 0)
		return err;

	if (new_creds)
		old_creds = override_creds(new_creds);

	ctx->upper = ctx->temp = NULL;
	err = ops->prepare(ctx);

	if (new_creds) {
		revert_creds(old_creds);
		put_cred(new_creds);
	}
	if (err < 0)
		goto out;

	/* err == 1 means we found an existing hardlinked upper inode */
	if (!err) {
		err = ovl_copy_up_inode(dentry, ctx->temp, ctx->lowerpath,
					ctx->stat);
		if (err)
			goto out_cancel;
	}

	if (ctx->stat->nlink) {
		err = ops->commit(ctx);
		if (err)
			goto out_cancel;

		newdentry = dget(ctx->upper);
	} else {
		err = -EEXIST;
		if (ctx->temp->d_inode->i_nlink != 1)
			goto out_cancel;

		/* Update with index dentry on r/o copy up */
		newdentry = dget(ctx->temp);
	}

	ovl_dentry_update(dentry, newdentry, !ctx->stat->nlink);
	err = ovl_inode_update(d_inode(dentry), d_inode(newdentry));
	if (err) {
		/* Broken hardlink - drop cache and return error */
		d_drop(dentry);
	}

out:
	dput(ctx->temp);
	dput(ctx->upper);
	return err;

out_cancel:
	ops->cancel(ctx);
	goto out;
}

/*
 * Copy up a single dentry
 *
 * All renames start with copy up of source if necessary.  The actual
 * rename will only proceed once the copy up was successful.  Copy up uses
 * upper parent i_mutex for exclusion.  Since rename can change d_parent it
 * is possible that the copy up will lock the old parent.  At that point
 * the file will have already been copied up anyway.
 */
static int ovl_copy_up_one(struct dentry *parent, struct dentry *dentry,
			   struct path *lowerpath, struct kstat *stat,
			   bool rocopyup)
{
	DEFINE_DELAYED_CALL(done);
	int err;
	struct path parentpath;
	struct dentry *lowerdentry = lowerpath->dentry;
	struct ovl_fs *ofs = dentry->d_sb->s_fs_info;
	bool indexed = ovl_indexdir(dentry->d_sb) && !S_ISDIR(stat->mode);
	struct ovl_copy_up_ctx ctx = {
		.dentry = dentry,
		.lowerpath = lowerpath,
		.stat = stat,
		.link = NULL,
		.tempdir = indexed ? ovl_indexdir(dentry->d_sb) :
				     ovl_workdir(dentry),
	};
	const struct ovl_copy_up_ops *ops;

	if (WARN_ON(rocopyup && !indexed))
		return 0;

	if (WARN_ON(!ctx.tempdir))
		return -EROFS;

	ovl_do_check_copy_up(lowerdentry);

	if (parent) {
		ovl_path_upper(parent, &parentpath);
		ctx.upperdir = parentpath.dentry;

		/*
		 * Mark parent "impure" because it may now contain non-pure
		 * upper.
		 */
		err = ovl_set_impure(parent, ctx.upperdir);
		if (err)
			return err;

		err = vfs_getattr(&parentpath, &ctx.pstat,
				  STATX_ATIME | STATX_MTIME,
				  AT_STATX_SYNC_AS_STAT);
		if (err)
			return err;
	}

	if (S_ISLNK(stat->mode)) {
		ctx.link = vfs_get_link(lowerdentry, &done);
		if (IS_ERR(ctx.link))
			return PTR_ERR(ctx.link);
	}

	/* Should we copyup with O_TMPFILE, with indexdir or with workdir? */
	if (indexed)
		ops = &ovl_copy_up_indexdir_ops;
	else if (S_ISREG(stat->mode) && ofs->tmpfile)
		ops = &ovl_copy_up_tmpfile_ops;
	else
		ops = &ovl_copy_up_workdir_ops;

	err = ops->aquire(&ctx);
	/* err < 0: interrupted, err > 0: raced with another copy-up */
	if (unlikely(err)) {
		pr_debug("%s(%pd2): aquire = %i\n", __func__, dentry, err);
		if (err > 0)
			err = 0;
		goto out_done;
	}
	if (rocopyup && ovl_dentry_ro_upper(dentry)) {
		/* Raced with another r/o copy up? */
		err = 0;
		goto out_unlock;
	}

	err = ovl_copy_up_locked(&ctx, ops);

out_unlock:
	ops->release(&ctx);
out_done:
	do_delayed_call(&done);

	return err;
}

int ovl_copy_up_flags(struct dentry *dentry, int flags, bool rocopyup)
{
	int err = 0;
	const struct cred *old_cred = ovl_override_creds(dentry->d_sb);

	while (!err) {
		struct dentry *next;
		struct dentry *parent = NULL;
		struct path lowerpath;
		struct kstat stat;
		enum ovl_path_type type = ovl_path_type(dentry);

		if (OVL_TYPE_UPPER(type) ||
		    (rocopyup && OVL_TYPE_RO_UPPER(type)))
			break;

		next = dget(dentry);
		/* find the topmost dentry not yet copied up */
		for (; !rocopyup;) {
			parent = dget_parent(next);

			type = ovl_path_type(parent);
			if (OVL_TYPE_UPPER(type))
				break;

			dput(next);
			next = parent;
		}

		ovl_path_lower(next, &lowerpath);
		err = vfs_getattr(&lowerpath, &stat,
				  STATX_BASIC_STATS, AT_STATX_SYNC_AS_STAT);
		/*
		 * maybe truncate regular file and maybe copy up as orphan
		 * index for read-only open. this has no effect on dirs.
		 */
		if (stat.nlink == 0) {
			err = -ENOENT;
		} else if (flags & O_TRUNC) {
			stat.size = 0;
		} else if (rocopyup) {
			WARN_ON(!S_ISREG(stat.mode));
			stat.nlink = 0;
		}

		if (!err)
			err = ovl_copy_up_one(parent, next, &lowerpath, &stat,
					      rocopyup);

		dput(parent);
		dput(next);
	}
	revert_creds(old_cred);

	return err;
}

int ovl_copy_up(struct dentry *dentry)
{
	return ovl_copy_up_flags(dentry, 0, false);
}
