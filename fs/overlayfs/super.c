/*
 *
 * Copyright (C) 2011 Novell Inc.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 as published by
 * the Free Software Foundation.
 */

#include <uapi/linux/magic.h>
#include <linux/fs.h>
#include <linux/namei.h>
#include <linux/xattr.h>
#include <linux/mount.h>
#include <linux/parser.h>
#include <linux/module.h>
#include <linux/statfs.h>
#include <linux/seq_file.h>
#include <linux/posix_acl_xattr.h>
#include "overlayfs.h"
#include "ovl_entry.h"

MODULE_AUTHOR("Miklos Szeredi <miklos@szeredi.hu>");
MODULE_DESCRIPTION("Overlay filesystem");
MODULE_LICENSE("GPL");


struct ovl_dir_cache;

#define OVL_MAX_STACK 500

static bool ovl_redirect_dir_def = IS_ENABLED(CONFIG_OVERLAY_FS_REDIRECT_DIR);
module_param_named(redirect_dir, ovl_redirect_dir_def, bool, 0644);
MODULE_PARM_DESC(ovl_redirect_dir_def,
		 "Default to on or off for the redirect_dir feature");

static bool ovl_index_def = IS_ENABLED(CONFIG_OVERLAY_FS_INDEX);
module_param_named(index, ovl_index_def, bool, 0644);
MODULE_PARM_DESC(ovl_index_def,
		 "Default to on or off for the inodes index feature");

void ovl_dentry_release(struct dentry *dentry)
{
	struct ovl_entry *oe = dentry->d_fsdata;

	if (oe) {
		unsigned int i;

		dput(oe->__snapdentry);
		for (i = 0; i < oe->numlower; i++)
			dput(oe->lowerstack[i].dentry);
		kfree_rcu(oe, rcu);
	}
}

int ovl_check_append_only(struct inode *inode, int flag)
{
	/*
	 * This test was moot in vfs may_open() because overlay inode does
	 * not have the S_APPEND flag, so re-check on real upper inode
	 */
	if (IS_APPEND(inode)) {
		if  ((flag & O_ACCMODE) != O_RDONLY && !(flag & O_APPEND))
			return -EPERM;
		if (flag & O_TRUNC)
			return -EPERM;
	}

	return 0;
}

static struct dentry *ovl_d_real(struct dentry *dentry,
				 const struct inode *inode,
				 unsigned int open_flags, unsigned int flags)
{
	struct dentry *real;
	bool rocopyup = !inode && ovl_consistent_fd(dentry->d_sb);
	int err;

	if (WARN_ON(open_flags && inode))
		return dentry;

	if (flags & D_REAL_UPPER)
		return ovl_dentry_upper(dentry);

	if (!d_is_reg(dentry)) {
		if (!inode || inode == d_inode(dentry))
			return dentry;
		goto bug;
	}

	if (open_flags || rocopyup) {
		err = ovl_open_maybe_copy_up(dentry, open_flags, rocopyup);
		if (err)
			return ERR_PTR(err);
	}

	real = ovl_dentry_upper(dentry);
	if (real && (!inode || inode == d_inode(real))) {
		if (!inode) {
			err = ovl_check_append_only(d_inode(real), open_flags);
			if (err)
				return ERR_PTR(err);
		}
		return real;
	}

	real = ovl_dentry_lower(dentry);
	if (!real)
		goto bug;

	/* Handle recursion */
	real = d_real(real, inode, open_flags, 0);

	if (!inode || inode == d_inode(real))
		return real;
bug:
	WARN(1, "ovl_d_real(%pd4, %s:%lu): real dentry not found\n", dentry,
	     inode ? inode->i_sb->s_id : "NULL", inode ? inode->i_ino : 0);
	return dentry;
}

static int ovl_dentry_revalidate(struct dentry *dentry, unsigned int flags)
{
	struct ovl_entry *oe = dentry->d_fsdata;
	unsigned int i;
	int ret = 1;

	for (i = 0; i < oe->numlower; i++) {
		struct dentry *d = oe->lowerstack[i].dentry;

		if (d->d_flags & DCACHE_OP_REVALIDATE) {
			ret = d->d_op->d_revalidate(d, flags);
			if (ret < 0)
				return ret;
			if (!ret) {
				if (!(flags & LOOKUP_RCU))
					d_invalidate(d);
				return -ESTALE;
			}
		}
	}
	return 1;
}

static int ovl_dentry_weak_revalidate(struct dentry *dentry, unsigned int flags)
{
	struct ovl_entry *oe = dentry->d_fsdata;
	unsigned int i;
	int ret = 1;

	for (i = 0; i < oe->numlower; i++) {
		struct dentry *d = oe->lowerstack[i].dentry;

		if (d->d_flags & DCACHE_OP_WEAK_REVALIDATE) {
			ret = d->d_op->d_weak_revalidate(d, flags);
			if (ret <= 0)
				break;
		}
	}
	return ret;
}

static const struct dentry_operations ovl_dentry_operations = {
	.d_release = ovl_dentry_release,
	.d_real = ovl_d_real,
};

static const struct dentry_operations ovl_reval_dentry_operations = {
	.d_release = ovl_dentry_release,
	.d_real = ovl_d_real,
	.d_revalidate = ovl_dentry_revalidate,
	.d_weak_revalidate = ovl_dentry_weak_revalidate,
};

static struct kmem_cache *ovl_inode_cachep;

static struct inode *ovl_alloc_inode(struct super_block *sb)
{
	struct ovl_inode *oi = kmem_cache_alloc(ovl_inode_cachep, GFP_KERNEL);

	oi->cache = NULL;
	oi->redirect = NULL;
	oi->version = 0;
	oi->flags = 0;
	oi->__upperdentry = NULL;
	oi->lower = NULL;
	mutex_init(&oi->lock);

	return &oi->vfs_inode;
}

static void ovl_i_callback(struct rcu_head *head)
{
	struct inode *inode = container_of(head, struct inode, i_rcu);

	kmem_cache_free(ovl_inode_cachep, OVL_I(inode));
}

static void ovl_destroy_inode(struct inode *inode)
{
	struct ovl_inode *oi = OVL_I(inode);

	dput(oi->__upperdentry);
	kfree(oi->redirect);
	ovl_dir_cache_free(inode);
	mutex_destroy(&oi->lock);

	call_rcu(&inode->i_rcu, ovl_i_callback);
}

static void ovl_put_super(struct super_block *sb)
{
	struct ovl_fs *ufs = sb->s_fs_info;
	unsigned i;

	dput(ufs->indexdir);
	dput(ufs->workdir);
	ovl_inuse_unlock(ufs->workbasedir);
	dput(ufs->workbasedir);
	if (ufs->upper_mnt)
		ovl_inuse_unlock(ufs->upper_mnt->mnt_root);
	mntput(ufs->upper_mnt);
	mntput(ufs->snap_mnt);
	mntput(ufs->__snapmnt);
	for (i = 0; i < ufs->numlower; i++)
		mntput(ufs->lower_mnt[i]);
	kfree(ufs->lower_mnt);

	kfree(ufs->config.snapshot);
	kfree(ufs->config.lowerdir);
	kfree(ufs->config.upperdir);
	kfree(ufs->config.workdir);
	put_cred(ufs->creator_cred);
	kfree(ufs);
}

static int ovl_sync_fs(struct super_block *sb, int wait)
{
	struct ovl_fs *ufs = sb->s_fs_info;
	struct super_block *upper_sb;
	int ret;

	if (!ufs->upper_mnt)
		return 0;
	upper_sb = ufs->upper_mnt->mnt_sb;
	if (!upper_sb->s_op->sync_fs)
		return 0;

	/* real inodes have already been synced by sync_filesystem(ovl_sb) */
	down_read(&upper_sb->s_umount);
	ret = upper_sb->s_op->sync_fs(upper_sb, wait);
	up_read(&upper_sb->s_umount);
	return ret;
}

static int ovl_freeze(struct super_block *sb)
{
	/* Make requested snapshot effective after fs is frozen */
	ovl_snapshot_barrier(sb);
	return 0;
}

static int ovl_unfreeze(struct super_block *sb)
{
	return 0;
}

/**
 * ovl_statfs
 * @sb: The overlayfs super block
 * @buf: The struct kstatfs to fill in with stats
 *
 * Get the filesystem statistics.  As writes always target the upper layer
 * filesystem pass the statfs to the upper filesystem (if it exists)
 */
static int ovl_statfs(struct dentry *dentry, struct kstatfs *buf)
{
	struct ovl_fs *ofs = dentry->d_sb->s_fs_info;
	struct dentry *root_dentry = dentry->d_sb->s_root;
	struct path path;
	int err;

	ovl_path_real(root_dentry, &path);

	err = vfs_statfs(&path, buf);
	if (!err) {
		buf->f_namelen = ofs->namelen;
		buf->f_type = OVERLAYFS_SUPER_MAGIC;
	}

	return err;
}

/* Will this overlay be forced to mount/remount ro? */
static bool ovl_force_readonly(struct ovl_fs *ufs)
{
	return (!ufs->upper_mnt || !ufs->workdir);
}

/**
 * ovl_show_options
 *
 * Prints the mount options for a given superblock.
 * Returns zero; does not fail.
 */
static int ovl_show_options(struct seq_file *m, struct dentry *dentry)
{
	struct super_block *sb = dentry->d_sb;
	struct ovl_fs *ufs = sb->s_fs_info;

	if (ufs->config.lowerdir)
		seq_show_option(m, "lowerdir", ufs->config.lowerdir);
	if (ufs->config.upperdir)
		seq_show_option(m, "upperdir", ufs->config.upperdir);
	if (ufs->config.workdir)
		seq_show_option(m, "workdir", ufs->config.workdir);
	if (ufs->config.snapshot)
		seq_show_option(m, "snapshot", ufs->config.snapshot);
	else if (ovl_is_snapshot_fs_type(sb))
		seq_puts(m, ",nosnapshot");
	if (ufs->config.default_permissions)
		seq_puts(m, ",default_permissions");
	if (ufs->config.redirect_dir != ovl_redirect_dir_def)
		seq_printf(m, ",redirect_dir=%s",
			   ufs->config.redirect_dir ? "on" : "off");
	if (ufs->config.lowerdir && ufs->config.index != ovl_index_def)
		seq_printf(m, ",index=%s",
			   ufs->config.index == OVL_INDEX_ALL ? "all" :
			   (ufs->config.index ? "on" : "off"));
	if (ufs->config.verify_dir)
		seq_puts(m, ",verify_dir");
	if (ufs->config.consistent_fd)
		seq_puts(m, ",consistent_fd");
	return 0;
}

static int ovl_remount(struct super_block *sb, int *flags, char *data)
{
	struct ovl_fs *ufs = sb->s_fs_info;

	if (ovl_is_snapshot_fs_type(sb))
		return ovl_snapshot_remount(sb, flags, data);

	if (*flags & MS_RDONLY)
		return 0;

	if (!ufs->numlower)
		return 0;

	if (ovl_force_readonly(ufs))
		return -EROFS;

	return 0;
}

static const struct super_operations ovl_super_operations = {
	.alloc_inode	= ovl_alloc_inode,
	.destroy_inode	= ovl_destroy_inode,
	.drop_inode	= generic_delete_inode,
	.put_super	= ovl_put_super,
	.sync_fs	= ovl_sync_fs,
	.freeze_fs	= ovl_freeze,
	.unfreeze_fs	= ovl_unfreeze,
	.statfs		= ovl_statfs,
	.show_options	= ovl_show_options,
	.remount_fs	= ovl_remount,
};

enum {
	OPT_LOWERDIR,
	OPT_UPPERDIR,
	OPT_WORKDIR,
	OPT_DEFAULT_PERMISSIONS,
	OPT_REDIRECT_DIR_ON,
	OPT_REDIRECT_DIR_OFF,
	OPT_INDEX_ON,
	OPT_INDEX_OFF,
	OPT_INDEX_ALL,
	OPT_VERIFY_DIR,
	OPT_CONSISTENT_FD,
	/* mount options that can be changed on remount: */
	OPT_REMOUNT_FIRST,
	OPT_SNAPSHOT = OPT_REMOUNT_FIRST,
	OPT_NOSNAPSHOT,
	OPT_ERR,
};

static const match_table_t ovl_tokens = {
	{OPT_LOWERDIR,			"lowerdir=%s"},
	{OPT_UPPERDIR,			"upperdir=%s"},
	{OPT_WORKDIR,			"workdir=%s"},
	{OPT_DEFAULT_PERMISSIONS,	"default_permissions"},
	{OPT_REDIRECT_DIR_ON,		"redirect_dir=on"},
	{OPT_REDIRECT_DIR_OFF,		"redirect_dir=off"},
	{OPT_INDEX_ON,			"index=on"},
	{OPT_INDEX_OFF,			"index=off"},
	{OPT_INDEX_ALL,			"index=all"},
	{OPT_VERIFY_DIR,		"verify_dir"},
	{OPT_CONSISTENT_FD,		"consistent_fd"},
	{OPT_SNAPSHOT,			"snapshot=%s"},
	{OPT_NOSNAPSHOT,		"nosnapshot"},
	{OPT_ERR,			NULL}
};

static char *ovl_next_opt(char **s)
{
	char *sbegin = *s;
	char *p;

	if (sbegin == NULL)
		return NULL;

	for (p = sbegin; *p; p++) {
		if (*p == '\\') {
			p++;
			if (!*p)
				break;
		} else if (*p == ',') {
			*p = '\0';
			*s = p + 1;
			return sbegin;
		}
	}
	*s = NULL;
	return sbegin;
}

int ovl_parse_opt(char *opt, struct ovl_config *config, bool remount)
{
	char *p;

	while ((p = ovl_next_opt(&opt)) != NULL) {
		int token;
		substring_t args[MAX_OPT_ARGS];

		if (!*p)
			continue;

		token = match_token(p, ovl_tokens, args);
		/* Ignore options that cannot be changed on remount */
		if (remount && token < OPT_REMOUNT_FIRST)
			continue;

		switch (token) {
		case OPT_UPPERDIR:
			kfree(config->upperdir);
			config->upperdir = match_strdup(&args[0]);
			if (!config->upperdir)
				return -ENOMEM;
			break;

		case OPT_LOWERDIR:
			kfree(config->lowerdir);
			config->lowerdir = match_strdup(&args[0]);
			if (!config->lowerdir)
				return -ENOMEM;
			break;

		case OPT_WORKDIR:
			kfree(config->workdir);
			config->workdir = match_strdup(&args[0]);
			if (!config->workdir)
				return -ENOMEM;
			break;

		case OPT_DEFAULT_PERMISSIONS:
			config->default_permissions = true;
			break;

		case OPT_REDIRECT_DIR_ON:
			config->redirect_dir = true;
			break;

		case OPT_REDIRECT_DIR_OFF:
			config->redirect_dir = false;
			break;

		case OPT_INDEX_ON:
			config->index = OVL_INDEX_NLINK;
			break;

		case OPT_INDEX_OFF:
			config->index = 0;
			break;

		case OPT_INDEX_ALL:
			config->index = OVL_INDEX_ALL;
			break;

		case OPT_VERIFY_DIR:
			config->verify_dir = true;
			break;

		case OPT_CONSISTENT_FD:
			config->consistent_fd = true;
			break;

#ifdef CONFIG_OVERLAY_FS_SNAPSHOT
		case OPT_SNAPSHOT:
			kfree(config->snapshot);
			config->snapshot = match_strdup(&args[0]);
			if (!config->snapshot)
				return -ENOMEM;
			break;

		case OPT_NOSNAPSHOT:
			kfree(config->snapshot);
			config->snapshot = NULL;
			break;
#endif
		default:
			pr_err("overlayfs: unrecognized mount option \"%s\" or missing value\n", p);
			return -EINVAL;
		}
	}

	if (!config->upperdir && !remount) {
		/* Workdir is useless in non-upper mount */
		if (config->workdir) {
			pr_info("overlayfs: option \"workdir=%s\" is useless in a non-upper mount, ignore\n",
				config->workdir);
			kfree(config->workdir);
			config->workdir = NULL;
		}

		if (config->redirect_dir != ovl_redirect_dir_def ||
		    config->index != ovl_index_def ||
		    config->verify_dir || config->consistent_fd) {
			pr_info("overlayfs: options \"redirect_dir\", \"verify_dir\" and \"index\" are useless in a non-upper mount, ignore\n");
			config->redirect_dir = ovl_redirect_dir_def;
			config->index = ovl_index_def;
			config->verify_dir = false;
			config->consistent_fd = false;
		}
	}

	return 0;
}

#define OVL_WORKDIR_NAME "work"
#define OVL_INDEXDIR_NAME "index"

static struct dentry *ovl_workdir_create(struct super_block *sb,
					 struct ovl_fs *ufs,
					 struct dentry *dentry,
					 const char *name, bool persist)
{
	struct inode *dir = dentry->d_inode;
	struct vfsmount *mnt = ufs->upper_mnt;
	struct dentry *work;
	int err;
	bool retried = false;
	bool locked = false;

	err = mnt_want_write(mnt);
	if (err)
		goto out_err;

	inode_lock_nested(dir, I_MUTEX_PARENT);
	locked = true;

retry:
	work = lookup_one_len(name, dentry, strlen(name));

	if (!IS_ERR(work)) {
		struct iattr attr = {
			.ia_valid = ATTR_MODE,
			.ia_mode = S_IFDIR | 0,
		};

		if (work->d_inode) {
			err = -EEXIST;
			if (retried)
				goto out_dput;

			if (persist)
				goto out_unlock;

			retried = true;
			ovl_workdir_cleanup(dir, mnt, work, 0);
			dput(work);
			goto retry;
		}

		err = ovl_create_real(dir, work,
				      &(struct cattr){.mode = S_IFDIR | 0},
				      NULL, true);
		if (err)
			goto out_dput;

		/*
		 * Try to remove POSIX ACL xattrs from workdir.  We are good if:
		 *
		 * a) success (there was a POSIX ACL xattr and was removed)
		 * b) -ENODATA (there was no POSIX ACL xattr)
		 * c) -EOPNOTSUPP (POSIX ACL xattrs are not supported)
		 *
		 * There are various other error values that could effectively
		 * mean that the xattr doesn't exist (e.g. -ERANGE is returned
		 * if the xattr name is too long), but the set of filesystems
		 * allowed as upper are limited to "normal" ones, where checking
		 * for the above two errors is sufficient.
		 */
		err = vfs_removexattr(work, XATTR_NAME_POSIX_ACL_DEFAULT);
		if (err && err != -ENODATA && err != -EOPNOTSUPP)
			goto out_dput;

		err = vfs_removexattr(work, XATTR_NAME_POSIX_ACL_ACCESS);
		if (err && err != -ENODATA && err != -EOPNOTSUPP)
			goto out_dput;

		/* Clear any inherited mode bits */
		inode_lock(work->d_inode);
		err = notify_change(work, &attr, NULL);
		inode_unlock(work->d_inode);
		if (err)
			goto out_dput;
	} else {
		err = PTR_ERR(work);
		goto out_err;
	}
out_unlock:
	mnt_drop_write(mnt);
	if (locked)
		inode_unlock(dir);

	return work;

out_dput:
	dput(work);
out_err:
	pr_warn("overlayfs: failed to create directory %s/%s (errno: %i); mounting read-only\n",
		ufs->config.workdir, name, -err);
	sb->s_flags |= MS_RDONLY;
	work = NULL;
	goto out_unlock;
}

static int ovl_mount_dir_noesc(const char *name, struct path *path)
{
	int err = -EINVAL;

	if (!*name) {
		pr_err("overlayfs: empty lowerdir\n");
		goto out;
	}
	err = kern_path(name, LOOKUP_FOLLOW, path);
	if (err) {
		pr_err("overlayfs: failed to resolve '%s': %i\n", name, err);
		goto out;
	}
	err = -EINVAL;
	if (ovl_dentry_weird(path->dentry)) {
		pr_err("overlayfs: filesystem on '%s' not supported\n", name);
		goto out_put;
	}
	if (!d_is_dir(path->dentry)) {
		pr_err("overlayfs: '%s' not a directory\n", name);
		goto out_put;
	}
	return 0;

out_put:
	path_put(path);
out:
	return err;
}

static int ovl_mount_dir(const char *name, struct path *path)
{
	int err = -ENOMEM;
	char *tmp = kstrdup(name, GFP_KERNEL);

	if (tmp) {
		ovl_unescape(tmp);
		err = ovl_mount_dir_noesc(tmp, path);

		if (!err)
			if (ovl_dentry_remote(path->dentry)) {
				pr_err("overlayfs: filesystem on '%s' not supported as upperdir\n",
				       tmp);
				path_put(path);
				err = -EINVAL;
			}
		kfree(tmp);
	}
	return err;
}

static int ovl_check_namelen(struct path *path, struct ovl_fs *ofs,
			     const char *name)
{
	struct kstatfs statfs;
	int err = vfs_statfs(path, &statfs);

	if (err)
		pr_err("overlayfs: statfs failed on '%s'\n", name);
	else
		ofs->namelen = max(ofs->namelen, statfs.f_namelen);

	return err;
}

int ovl_lower_dir(const char *name, struct path *path,
		  struct ovl_fs *ofs, int *stack_depth, bool *remote)
{
	int err;

	err = ovl_mount_dir_noesc(name, path);
	if (err)
		goto out;

	err = ovl_check_namelen(path, ofs, name);
	if (err)
		goto out_put;

	*stack_depth = max(*stack_depth, path->mnt->mnt_sb->s_stack_depth);

	if (ovl_dentry_remote(path->dentry))
		*remote = true;

	/*
	 * The features inodes index and verify_dir need to decode lower
	 * file handles, so they require that all lower layers support them.
	 */
	if (!ovl_can_decode_fh(path->dentry->d_sb)) {
		if (ofs->config.verify_dir) {
			err = -EOPNOTSUPP;
			pr_err("overlayfs: fs on '%s' does not support file handles, option \"verify_dir\" not supported.\n", name);
			goto out_put;
		}

		if (ofs->config.index) {
			ofs->config.index = 0;
			pr_warn("overlayfs: fs on '%s' does not support file handles, falling back to index=off.\n", name);
		}
	}

	return 0;

out_put:
	path_put(path);
out:
	return err;
}

/* Workdir should not be subdir of upperdir and vice versa */
static bool ovl_workdir_ok(struct dentry *workdir, struct dentry *upperdir)
{
	bool ok = false;

	if (workdir != upperdir) {
		ok = (lock_rename(workdir, upperdir) == NULL);
		unlock_rename(workdir, upperdir);
	}
	return ok;
}

static unsigned int ovl_split_lowerdirs(char *str)
{
	unsigned int ctr = 1;
	char *s, *d;

	for (s = d = str;; s++, d++) {
		if (*s == '\\') {
			s++;
		} else if (*s == ':') {
			*d = '\0';
			ctr++;
			continue;
		}
		*d = *s;
		if (!*s)
			break;
	}
	return ctr;
}

static int __maybe_unused
ovl_posix_acl_xattr_get(const struct xattr_handler *handler,
			struct dentry *dentry, struct inode *inode,
			const char *name, void *buffer, size_t size)
{
	return ovl_xattr_get(dentry, inode, handler->name, buffer, size);
}

static int __maybe_unused
ovl_posix_acl_xattr_set(const struct xattr_handler *handler,
			struct dentry *dentry, struct inode *inode,
			const char *name, const void *value,
			size_t size, int flags)
{
	struct dentry *workdir = ovl_workdir(dentry);
	struct inode *realinode = ovl_inode_real(inode);
	struct posix_acl *acl = NULL;
	int err;

	/* Check that everything is OK before copy-up */
	if (value) {
		acl = posix_acl_from_xattr(&init_user_ns, value, size);
		if (IS_ERR(acl))
			return PTR_ERR(acl);
	}
	err = -EOPNOTSUPP;
	if (workdir && !IS_POSIXACL(d_inode(workdir)))
		goto out_acl_release;
	if (!realinode->i_op->set_acl)
		goto out_acl_release;
	if (handler->flags == ACL_TYPE_DEFAULT && !S_ISDIR(inode->i_mode)) {
		err = acl ? -EACCES : 0;
		goto out_acl_release;
	}
	err = -EPERM;
	if (!inode_owner_or_capable(inode))
		goto out_acl_release;

	posix_acl_release(acl);

	/*
	 * Check if sgid bit needs to be cleared (actual setacl operation will
	 * be done with mounter's capabilities and so that won't do it for us).
	 */
	if (unlikely(inode->i_mode & S_ISGID) &&
	    handler->flags == ACL_TYPE_ACCESS &&
	    !in_group_p(inode->i_gid) &&
	    !capable_wrt_inode_uidgid(inode, CAP_FSETID)) {
		struct iattr iattr = { .ia_valid = ATTR_KILL_SGID };

		err = ovl_setattr(dentry, &iattr);
		if (err)
			return err;
	}

	err = ovl_xattr_set(dentry, inode, handler->name, value, size, flags);
	if (!err)
		ovl_copyattr(ovl_inode_real(inode), inode);

	return err;

out_acl_release:
	posix_acl_release(acl);
	return err;
}

static int ovl_own_xattr_get(const struct xattr_handler *handler,
			     struct dentry *dentry, struct inode *inode,
			     const char *name, void *buffer, size_t size)
{
	return -EOPNOTSUPP;
}

static int ovl_own_xattr_set(const struct xattr_handler *handler,
			     struct dentry *dentry, struct inode *inode,
			     const char *name, const void *value,
			     size_t size, int flags)
{
	return -EOPNOTSUPP;
}

static int ovl_other_xattr_get(const struct xattr_handler *handler,
			       struct dentry *dentry, struct inode *inode,
			       const char *name, void *buffer, size_t size)
{
	return ovl_xattr_get(dentry, inode, name, buffer, size);
}

static int ovl_other_xattr_set(const struct xattr_handler *handler,
			       struct dentry *dentry, struct inode *inode,
			       const char *name, const void *value,
			       size_t size, int flags)
{
	return ovl_xattr_set(dentry, inode, name, value, size, flags);
}

static const struct xattr_handler __maybe_unused
ovl_posix_acl_access_xattr_handler = {
	.name = XATTR_NAME_POSIX_ACL_ACCESS,
	.flags = ACL_TYPE_ACCESS,
	.get = ovl_posix_acl_xattr_get,
	.set = ovl_posix_acl_xattr_set,
};

static const struct xattr_handler __maybe_unused
ovl_posix_acl_default_xattr_handler = {
	.name = XATTR_NAME_POSIX_ACL_DEFAULT,
	.flags = ACL_TYPE_DEFAULT,
	.get = ovl_posix_acl_xattr_get,
	.set = ovl_posix_acl_xattr_set,
};

static const struct xattr_handler ovl_own_xattr_handler = {
	.prefix	= OVL_XATTR_PREFIX,
	.get = ovl_own_xattr_get,
	.set = ovl_own_xattr_set,
};

static const struct xattr_handler ovl_other_xattr_handler = {
	.prefix	= "", /* catch all */
	.get = ovl_other_xattr_get,
	.set = ovl_other_xattr_set,
};

static const struct xattr_handler *ovl_xattr_handlers[] = {
#ifdef CONFIG_FS_POSIX_ACL
	&ovl_posix_acl_access_xattr_handler,
	&ovl_posix_acl_default_xattr_handler,
#endif
	&ovl_own_xattr_handler,
	&ovl_other_xattr_handler,
	NULL
};

static int ovl_fill_super(struct super_block *sb, void *data, int silent)
{
	struct path upperpath = { };
	struct path snappath = { };
	struct path workpath = { };
	struct dentry *root_dentry;
	struct ovl_entry *oe;
	struct ovl_fs *ufs;
	struct path *stack = NULL;
	char *lowertmp = NULL;
	char *lower;
	unsigned int numlower = 0;
	unsigned int stacklen = 0;
	unsigned int i;
	bool remote = false;
	struct cred *cred;
	int err;

	err = -ENOMEM;
	ufs = kzalloc(sizeof(struct ovl_fs), GFP_KERNEL);
	if (!ufs)
		goto out;

	ufs->config.redirect_dir = ovl_redirect_dir_def;
	ufs->config.index = (enum ovl_index) ovl_index_def;
	err = ovl_parse_opt((char *) data, &ufs->config, false);
	if (err)
		goto out_free_config;

	err = -EINVAL;
	if (!ovl_is_snapshot_fs_type(sb) && !ufs->config.lowerdir) {
		if (!silent)
			pr_err("overlayfs: missing 'lowerdir'\n");
		goto out_free_config;
	}

	sb->s_stack_depth = 0;
	sb->s_maxbytes = MAX_LFS_FILESIZE;

	if (ovl_is_snapshot_fs_type(sb)) {
		if (!ufs->config.upperdir) {
			if (!silent)
				pr_err("overlayfs: fs type 'snapshot' requires 'upperdir'\n");
			goto out_free_config;
		}

		if (ufs->config.lowerdir) {
			if (!silent)
				pr_err("overlayfs: fs type 'snapshot' requires no 'lowerdir'\n");
			goto out_free_config;
		}

		ufs->config.index = false;

		/*
		 * snapshot mount may be remounted later with underlying
		 * snapshot overlay. we must leave room in stack below us
		 * for that overlay, even if snapshot= mount option is not
		 * provided on the initial mount.
		 */
		sb->s_stack_depth = FILESYSTEM_MAX_STACK_DEPTH - 1;
	}

	if (ufs->config.upperdir) {
		if (!ovl_is_snapshot_fs_type(sb) && !ufs->config.workdir) {
			pr_err("overlayfs: missing 'workdir'\n");
			goto out_free_config;
		}

		err = ovl_mount_dir(ufs->config.upperdir, &upperpath);
		if (err)
			goto out_free_config;

		/* Upper fs should not be r/o */
		if (sb_rdonly(upperpath.mnt->mnt_sb)) {
			pr_err("overlayfs: upper fs is r/o, try multi-lower layers mount\n");
			err = -EINVAL;
			goto out_put_upperpath;
		}

		err = ovl_check_namelen(&upperpath, ufs, ufs->config.upperdir);
		if (err)
			goto out_put_upperpath;

		err = -EBUSY;
		if (!ovl_inuse_trylock(upperpath.dentry)) {
			pr_err("overlayfs: upperdir is in-use by another mount\n");
			goto out_put_upperpath;
		}

		sb->s_stack_depth = max(sb->s_stack_depth,
					upperpath.mnt->mnt_sb->s_stack_depth);
	}

	if (ufs->config.snapshot) {
		if (!ovl_is_snapshot_fs_type(sb)) {
			pr_err("overlayfs: option 'snapshot' requires fs type 'snapshot'\n");
			return -EINVAL;
		}

		err = ovl_snapshot_dir(ufs->config.snapshot, &snappath,
				       ufs, sb);
		if (err)
			goto out_put_upperpath;
	}

	if (ufs->config.workdir) {
		err = ovl_mount_dir(ufs->config.workdir, &workpath);
		if (err)
			goto out_unlock_upperdentry;

		err = -EINVAL;
		if (upperpath.mnt != workpath.mnt) {
			pr_err("overlayfs: workdir and upperdir must reside under the same mount\n");
			goto out_put_workpath;
		}
		if (!ovl_workdir_ok(workpath.dentry, upperpath.dentry)) {
			pr_err("overlayfs: workdir and upperdir must be separate subtrees\n");
			goto out_put_workpath;
		}

		err = -EBUSY;
		if (!ovl_inuse_trylock(workpath.dentry)) {
			pr_err("overlayfs: workdir is in-use by another mount\n");
			goto out_put_workpath;
		}

		ufs->workbasedir = workpath.dentry;
	}

	if (ufs->config.lowerdir) {
		err = -ENOMEM;
		lowertmp = kstrdup(ufs->config.lowerdir, GFP_KERNEL);
		if (!lowertmp)
			goto out_unlock_workdentry;

		stacklen = ovl_split_lowerdirs(lowertmp);
	}

	err = -EINVAL;
	if (stacklen > OVL_MAX_STACK) {
		pr_err("overlayfs: too many lower directories, limit is %d\n",
		       OVL_MAX_STACK);
		goto out_free_lowertmp;
	} else if (!ufs->config.upperdir && stacklen == 1) {
		pr_err("overlayfs: at least 2 lowerdir are needed while upperdir nonexistent\n");
		goto out_free_lowertmp;
	} else if (!ufs->config.upperdir || !ufs->config.workdir) {
		ufs->config.index = false;
	}

	if (stacklen > 0) {
		err = -ENOMEM;
		stack = kcalloc(stacklen, sizeof(struct path), GFP_KERNEL);
		if (!stack)
			goto out_free_lowertmp;
	}
	err = -EINVAL;
	lower = lowertmp;
	for (numlower = 0; numlower < stacklen; numlower++) {
		err = ovl_lower_dir(lower, &stack[numlower], ufs,
				    &sb->s_stack_depth, &remote);
		if (err)
			goto out_put_lowerpath;

		lower = strchr(lower, '\0') + 1;
	}

	err = -EINVAL;
	sb->s_stack_depth++;
	if (sb->s_stack_depth > FILESYSTEM_MAX_STACK_DEPTH) {
		pr_err("overlayfs: maximum fs stacking depth exceeded\n");
		goto out_put_lowerpath;
	}

	if (ufs->config.upperdir) {
		ufs->upper_mnt = clone_private_mount(&upperpath);
		err = PTR_ERR(ufs->upper_mnt);
		if (IS_ERR(ufs->upper_mnt)) {
			pr_err("overlayfs: failed to clone upperpath\n");
			goto out_put_lowerpath;
		}

		/* Don't inherit atime flags */
		ufs->upper_mnt->mnt_flags &= ~(MNT_NOATIME | MNT_NODIRATIME | MNT_RELATIME);

		sb->s_time_gran = ufs->upper_mnt->mnt_sb->s_time_gran;
	}

	if (ufs->config.snapshot) {
		struct vfsmount *snapmnt = ovl_snapshot_mount(&snappath, ufs);

		if (IS_ERR(snapmnt))
			goto out_put_upper_mnt;

		/* Set both requested and effective snapshot overlay */
		ufs->snap_mnt = snapmnt;
		ufs->__snapmnt = mntget(snapmnt);
	}

	if (ufs->config.workdir) {
		ufs->workdir = ovl_workdir_create(sb, ufs, workpath.dentry,
						  OVL_WORKDIR_NAME, false);
		/*
		 * Upper should support d_type, else whiteouts are visible.
		 * Given workdir and upper are on same fs, we can do
		 * iterate_dir() on workdir. This check requires successful
		 * creation of workdir in previous step.
		 */
		if (ufs->workdir) {
			struct dentry *temp;

			err = ovl_check_d_type_supported(&workpath);
			if (err < 0)
				goto out_put_workdir;

			/*
			 * We allowed this configuration and don't want to
			 * break users over kernel upgrade. So warn instead
			 * of erroring out.
			 */
			if (!err)
				pr_warn("overlayfs: upper fs needs to support d_type.\n");

			/* Check if upper/work fs supports O_TMPFILE */
			temp = ovl_do_tmpfile(ufs->workdir, S_IFREG | 0);
			ufs->tmpfile = !IS_ERR(temp);
			if (ufs->tmpfile) {
				/* Check if upper/work supports clone */
				if (temp->d_inode && temp->d_inode->i_fop &&
				    temp->d_inode->i_fop->clone_file_range)
					ufs->cloneup = true;
				dput(temp);
			} else {
				pr_warn("overlayfs: upper fs does not support tmpfile.\n");
			}

			/*
			 * Check if upper/work fs supports trusted.overlay.*
			 * xattr
			 */
			err = ovl_do_setxattr(ufs->workdir, OVL_XATTR_OPAQUE,
					      "0", 1, 0);
			if (err) {
				ufs->noxattr = true;
				pr_warn("overlayfs: upper fs does not support xattr.\n");
				if (ufs->config.index ||
				    ufs->config.redirect_dir ||
				    ufs->config.verify_dir) {
					pr_warn("overlayfs: falling back to redirect_dir=off, verify_dir=off, index=off.\n");
					ufs->config.index = 0;
					ufs->config.redirect_dir = false;
					ufs->config.verify_dir = false;
				}
			} else {
				vfs_removexattr(ufs->workdir, OVL_XATTR_OPAQUE);
			}

			/* Check if upper/work fs supports file handles */
			if (ufs->config.index &&
			    !ovl_can_decode_fh(ufs->workdir->d_sb)) {
				ufs->config.index = 0;
				pr_warn("overlayfs: upper fs does not support file handles, falling back to index=off.\n");
			}
		}
	}

	if (numlower > 0) {
		err = -ENOMEM;
		ufs->lower_mnt = kcalloc(numlower, sizeof(struct vfsmount *),
					 GFP_KERNEL);
		if (ufs->lower_mnt == NULL)
			goto out_put_workdir;
	} else {
		ufs->same_sb = ufs->upper_mnt->mnt_sb;
		ufs->config.index = false;
	}

	for (i = 0; i < numlower; i++) {
		struct vfsmount *mnt = clone_private_mount(&stack[i]);

		err = PTR_ERR(mnt);
		if (IS_ERR(mnt)) {
			pr_err("overlayfs: failed to clone lowerpath\n");
			goto out_put_lower_mnt;
		}

		/*
		 * Make lower_mnt R/O.  That way fchmod/fchown on lower file
		 * will fail instead of modifying lower fs.
		 */
		mnt->mnt_flags |= MNT_READONLY | MNT_NOATIME;

		ufs->lower_mnt[ufs->numlower] = mnt;
		ufs->numlower++;

		/* Check if all lower layers are on same sb */
		if (i == 0)
			ufs->same_sb = mnt->mnt_sb;
		else if (ufs->same_sb != mnt->mnt_sb)
			ufs->same_sb = NULL;
	}

	/* If the upper fs is nonexistent, we mark overlayfs r/o too */
	if (!ufs->upper_mnt)
		sb->s_flags |= MS_RDONLY;
	else if (ufs->upper_mnt->mnt_sb != ufs->same_sb)
		ufs->same_sb = NULL;

	if (!ufs->same_sb)
		ufs->cloneup = false;

	if (!(ovl_force_readonly(ufs)) && ufs->config.index) {
		ufs->indexdir = ovl_workdir_create(sb, ufs, workpath.dentry,
						   OVL_INDEXDIR_NAME, true);
		if (ufs->indexdir) {
			/* Verify upper root is index dir origin */
			err = ovl_verify_origin(ufs->indexdir, ufs->upper_mnt,
						upperpath.dentry, true, true);
			if (err)
				pr_err("overlayfs: failed to verify index dir origin\n");

			/* Cleanup bad/stale/orphan index entries */
			if (!err)
				err = ovl_indexdir_cleanup(ufs->indexdir,
							   ufs->upper_mnt,
							   stack, numlower);
		}
		if (err || !ufs->indexdir)
			pr_warn("overlayfs: try deleting index dir or mounting with '-o index=off' to disable inodes index.\n");
		if (err)
			goto out_put_indexdir;
	}

	/* Show index=off/on in /proc/mounts for any of the reasons above */
	if (!ufs->indexdir)
		ufs->config.index = 0;

	if (ufs->config.verify_dir || ufs->indexdir) {
		/* Verify lower root is upper root origin */
		err = ovl_verify_origin(upperpath.dentry, ufs->lower_mnt[0],
					stack[0].dentry, false, true);
		if (err) {
			pr_err("overlayfs: failed to verify upper root origin\n");
			goto out_put_indexdir;
		}
	}

	/*
	 * Copy on read for consistent fd depends on index dir, which depends
	 * on !ovl_force_readonly.
	 * On ro mount, fd is always consistent, but if overlay can be
	 * later remounted rw, we need to copy on read anyway, so that
	 * ro fd that was opened during ro mount will be consistent with
	 * rw fd that is opened after remount rw.
	 */
	if (!ufs->indexdir)
		ufs->config.consistent_fd = false;

	if (ovl_is_snapshot_fs_type(sb))
		sb->s_d_op = &ovl_snapshot_dentry_operations;
	else if (remote)
		sb->s_d_op = &ovl_reval_dentry_operations;
	else
		sb->s_d_op = &ovl_dentry_operations;

	err = -ENOMEM;
	ufs->creator_cred = cred = prepare_creds();
	if (!cred)
		goto out_put_indexdir;

	/* Never override disk quota limits or use reserved space */
	cap_lower(cred->cap_effective, CAP_SYS_RESOURCE);

	err = -ENOMEM;
	oe = ovl_alloc_entry(numlower);
	if (!oe)
		goto out_put_cred;

	sb->s_magic = OVERLAYFS_SUPER_MAGIC;
	sb->s_op = &ovl_super_operations;
	sb->s_xattr = ovl_xattr_handlers;
	sb->s_fs_info = ufs;
	sb->s_flags |= MS_POSIXACL | MS_NOREMOTELOCK;

	root_dentry = d_make_root(ovl_new_inode(sb, S_IFDIR, 0));
	if (!root_dentry)
		goto out_free_oe;

	mntput(upperpath.mnt);
	mntput(snappath.mnt);
	for (i = 0; i < numlower; i++)
		mntput(stack[i].mnt);
	mntput(workpath.mnt);
	kfree(lowertmp);

	if (upperpath.dentry) {
		oe->has_upper = true;
		if (ovl_is_impuredir(upperpath.dentry))
			ovl_set_flag(OVL_IMPURE, d_inode(root_dentry));
	}
	oe->__snapdentry = snappath.dentry;
	for (i = 0; i < numlower; i++) {
		oe->lowerstack[i].dentry = stack[i].dentry;
		oe->lowerstack[i].mnt = ufs->lower_mnt[i];
	}
	kfree(stack);

	root_dentry->d_fsdata = oe;

	ovl_inode_init(d_inode(root_dentry), upperpath.dentry,
		       ovl_dentry_lower(root_dentry));

	sb->s_root = root_dentry;

	return 0;

out_free_oe:
	kfree(oe);
out_put_cred:
	put_cred(ufs->creator_cred);
out_put_indexdir:
	dput(ufs->indexdir);
out_put_lower_mnt:
	for (i = 0; i < ufs->numlower; i++)
		mntput(ufs->lower_mnt[i]);
	kfree(ufs->lower_mnt);
out_put_workdir:
	dput(ufs->workdir);
out_put_upper_mnt:
	mntput(ufs->upper_mnt);
	mntput(ufs->snap_mnt);
	mntput(ufs->__snapmnt);
out_put_lowerpath:
	for (i = 0; i < numlower; i++)
		path_put(&stack[i]);
	kfree(stack);
out_free_lowertmp:
	kfree(lowertmp);
out_unlock_workdentry:
	ovl_inuse_unlock(workpath.dentry);
out_put_workpath:
	path_put(&workpath);
out_unlock_upperdentry:
	ovl_inuse_unlock(upperpath.dentry);
out_put_upperpath:
	path_put(&upperpath);
	path_put(&snappath);
out_free_config:
	kfree(ufs->config.snapshot);
	kfree(ufs->config.lowerdir);
	kfree(ufs->config.upperdir);
	kfree(ufs->config.workdir);
	kfree(ufs);
out:
	return err;
}

struct dentry *ovl_mount(struct file_system_type *fs_type, int flags,
			 const char *dev_name, void *raw_data)
{
	return mount_nodev(fs_type, flags, raw_data, ovl_fill_super);
}

static struct file_system_type ovl_fs_type = {
	.owner		= THIS_MODULE,
	.name		= "overlay",
	.mount		= ovl_mount,
	.kill_sb	= kill_anon_super,
};
MODULE_ALIAS_FS("overlay");

static void ovl_inode_init_once(void *foo)
{
	struct ovl_inode *oi = foo;

	inode_init_once(&oi->vfs_inode);
}

static int __init ovl_init(void)
{
	int err;

	ovl_inode_cachep = kmem_cache_create("ovl_inode",
					     sizeof(struct ovl_inode), 0,
					     (SLAB_RECLAIM_ACCOUNT|
					      SLAB_MEM_SPREAD|SLAB_ACCOUNT),
					     ovl_inode_init_once);
	if (ovl_inode_cachep == NULL)
		return -ENOMEM;

	err = register_filesystem(&ovl_fs_type);
	if (err)
		kmem_cache_destroy(ovl_inode_cachep);

	/* This module may also serve mount -t snapshot */
	ovl_snapshot_fs_register();

	return err;
}

static void __exit ovl_exit(void)
{
	ovl_snapshot_fs_unregister();
	unregister_filesystem(&ovl_fs_type);

	/*
	 * Make sure all delayed rcu free inodes are flushed before we
	 * destroy cache.
	 */
	rcu_barrier();
	kmem_cache_destroy(ovl_inode_cachep);

}

module_init(ovl_init);
module_exit(ovl_exit);
