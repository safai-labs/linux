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
	bool consistent_fd;
};

/* private information held for overlayfs's superblock */
struct ovl_fs {
	struct vfsmount *upper_mnt;
	unsigned numlower;
	struct vfsmount **lower_mnt;
	struct dentry *workdir;
	long namelen;
	/* pathnames of lower and upper dirs, for show_options */
	struct ovl_config config;
	/* creds of process who forced instantiation of super block */
	const struct cred *creator_cred;
	bool tmpfile;	/* upper supports O_TMPFILE */
	bool cloneup;	/* can clone from lower to upper */
	bool rocopyup;	/* copy up on open for read */
	wait_queue_head_t copyup_wq;
	/* sb common to all (or all lower) layers */
	struct super_block *same_lower_sb;
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
			bool copying;
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
