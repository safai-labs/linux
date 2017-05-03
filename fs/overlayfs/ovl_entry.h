/*
 *
 * Copyright (C) 2011 Novell Inc.
 * Copyright (C) 2016 Red Hat, Inc.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 as published by
 * the Free Software Foundation.
 */

struct ovl_config {
	char *lowerdir;
	char *upperdir;
	char *workdir;
	bool default_permissions;
	bool redirect_dir;
	bool index;
	bool consistent_fd;
};

/* private information held for overlayfs's superblock */
struct ovl_fs {
	struct vfsmount *upper_mnt;
	unsigned numlower;
	struct vfsmount **lower_mnt;
	/* workbasedir is the path at workdir= mount option */
	struct dentry *workbasedir;
	/* workdir is the 'work' directory under workbasedir */
	struct dentry *workdir;
	/* index directory listing overlay inodes by origin file handle */
	struct dentry *indexdir;
	long namelen;
	/* pathnames of lower and upper dirs, for show_options */
	struct ovl_config config;
	/* creds of process who forced instantiation of super block */
	const struct cred *creator_cred;
	bool tmpfile;
	bool noxattr;
	bool cloneup;
	bool rocopyup;	/* copy up on open for read */
	/* sb common to all layers */
	struct super_block *same_sb;
};

enum ovl_path_type;

/* private information held for every overlayfs dentry */
struct ovl_entry {
	struct dentry *__upperdentry;
	union {
		struct dentry *__roupperdentry; /* regular file */
		struct ovl_dir_cache *cache; /* directory */
	};
	union {
		struct {
			u64 version;
			const char *redirect;
		};
		struct rcu_head rcu;
	};
	enum ovl_path_type __type;
	unsigned numlower;
	struct path lowerstack[];
};

struct ovl_entry *ovl_alloc_entry(unsigned int numlower);

static inline struct dentry *ovl_upperdentry_dereference(struct ovl_entry *oe)
{
	return lockless_dereference(oe->__upperdentry);
}

static inline struct dentry *ovl_roupperdentry_dereference(struct ovl_entry *oe)
{
	return lockless_dereference(oe->__roupperdentry);
}

/* private information embedded in every overlayfs inode */
struct ovl_inode_info {
	struct inode *__upperinode;
	struct inode *lowerinode;
};

struct ovl_inode {
	/* keep this first */
	struct inode vfs_inode;
	struct ovl_inode_info info;
	/* synchronize copy up and more */
	struct mutex oi_lock;
};

static inline struct ovl_inode *OVL_I(struct inode *inode)
{
	return (struct ovl_inode *) inode;
}

static inline struct ovl_inode_info *OVL_I_INFO(struct inode *inode)
{
	return &OVL_I(inode)->info;
}
