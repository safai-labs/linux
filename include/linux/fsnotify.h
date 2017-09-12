#ifndef _LINUX_FS_NOTIFY_H
#define _LINUX_FS_NOTIFY_H

/*
 * include/linux/fsnotify.h - generic hooks for filesystem notification, to
 * reduce in-source duplication from both dnotify and inotify.
 *
 * We don't compile any of this away in some complicated menagerie of ifdefs.
 * Instead, we rely on the code inside to optimize away as needed.
 *
 * (C) Copyright 2005 Robert Love
 */

#include <linux/fsnotify_backend.h>
#include <linux/audit.h>
#include <linux/slab.h>
#include <linux/bug.h>

/* Notify this dentry's parent about a child's events. */
static inline int fsnotify_parent(struct path *path, struct dentry *dentry, __u32 mask)
{
	if (!dentry)
		dentry = path->dentry;

	return __fsnotify_parent(path, dentry, mask);
}

/*
 * Notify this parent about a filename event (create,delete,rename).
 * Unlike fsnotify_parent(), the event will be reported regardless of the
 * FS_EVENT_ON_CHILD mask on the parent inode
 */
static inline int fsnotify_filename(struct dentry *parent, __u32 mask,
				    const unsigned char *file_name, u32 cookie)
{
	return fsnotify(d_inode(parent), mask, parent, FSNOTIFY_EVENT_DENTRY,
			file_name, cookie);
}

/*
 * Call fsnotify_filename() with parent and d_name of this dentry.
 * Safe to call with negative dentry, e.g. from fsnotify_nameremove()
 */
static inline int fsnotify_d_name(struct dentry *dentry, __u32 mask)
{
	return fsnotify_filename(dentry->d_parent, mask,
				 dentry->d_name.name, 0);
}

static inline void audit_dentry_child(const struct dentry *dentry,
				      const unsigned char type)
{
	audit_inode_child(d_inode(dentry->d_parent), dentry, type);
}

/* simple call site for access decisions */
static inline int fsnotify_perm(struct file *file, int mask)
{
	struct path *path = &file->f_path;
	/*
	 * Do not use file_inode() here or anywhere in this file to get the
	 * inode.  That would break *notity on overlayfs.
	 */
	struct inode *inode = path->dentry->d_inode;
	__u32 fsnotify_mask = 0;
	int ret;

	if (file->f_mode & FMODE_NONOTIFY)
		return 0;
	if (!(mask & (MAY_READ | MAY_OPEN)))
		return 0;
	if (mask & MAY_OPEN)
		fsnotify_mask = FS_OPEN_PERM;
	else if (mask & MAY_READ)
		fsnotify_mask = FS_ACCESS_PERM;
	else
		BUG();

	ret = fsnotify_parent(path, NULL, fsnotify_mask);
	if (ret)
		return ret;

	return fsnotify(inode, fsnotify_mask, path, FSNOTIFY_EVENT_PATH, NULL, 0);
}

/*
 * fsnotify_link_count - inode's link count changed
 */
static inline void fsnotify_link_count(struct inode *inode)
{
	fsnotify(inode, FS_ATTRIB, inode, FSNOTIFY_EVENT_INODE, NULL, 0);
}

/*
 * fsnotify_move - file old_name at old_dir was moved to new_name at new_dir
 */
static inline void fsnotify_move(struct dentry *old_dir,
				 struct dentry *new_dir,
				 const unsigned char *old_name,
				 int isdir, struct inode *target,
				 struct dentry *moved)
{
	struct inode *source = d_inode(moved);
	u32 fs_cookie = fsnotify_get_cookie();
	__u32 old_dir_mask = FS_MOVED_FROM;
	__u32 new_dir_mask = FS_MOVED_TO;
	const unsigned char *new_name = moved->d_name.name;

	if (d_inode(old_dir) == d_inode(new_dir)) {
		old_dir_mask |= FS_DN_RENAME;
		new_dir_mask |= FS_DN_RENAME;
	}

	if (isdir) {
		old_dir_mask |= FS_ISDIR;
		new_dir_mask |= FS_ISDIR;
	}

	fsnotify_filename(old_dir, old_dir_mask, old_name, fs_cookie);
	fsnotify_filename(new_dir, new_dir_mask, new_name, fs_cookie);

	if (target)
		fsnotify_link_count(target);

	if (source)
		fsnotify(source, FS_MOVE_SELF, moved, FSNOTIFY_EVENT_DENTRY,
			 NULL, 0);
	audit_dentry_child(moved, AUDIT_TYPE_CHILD_CREATE);
}

/*
 * fsnotify_inode_delete - and inode is being evicted from cache, clean up is needed
 */
static inline void fsnotify_inode_delete(struct inode *inode)
{
	__fsnotify_inode_delete(inode);
}

/*
 * fsnotify_vfsmount_delete - a vfsmount is being destroyed, clean up is needed
 */
static inline void fsnotify_vfsmount_delete(struct vfsmount *mnt)
{
	__fsnotify_vfsmount_delete(mnt);
}

/*
 * fsnotify_nameremove - a filename was removed from a directory
 */
static inline void fsnotify_nameremove(struct dentry *dentry, int isdir)
{
	__u32 mask = FS_DELETE;

	if (isdir)
		mask |= FS_ISDIR;

	fsnotify_d_name(dentry, mask);
}

/*
 * fsnotify_inoderemove - an inode is going away
 */
static inline void fsnotify_inoderemove(struct inode *inode)
{
	fsnotify(inode, FS_DELETE_SELF, inode, FSNOTIFY_EVENT_INODE, NULL, 0);
	__fsnotify_inode_delete(inode);
}

/*
 * fsnotify_create - 'name' was linked in
 */
static inline void fsnotify_create(struct dentry *dentry)
{
	audit_dentry_child(dentry, AUDIT_TYPE_CHILD_CREATE);

	fsnotify_d_name(dentry, FS_CREATE);
}

/*
 * fsnotify_link - new hardlink in 'inode' directory
 * Note: We have to pass also the linked inode ptr as some filesystems leave
 *   new_dentry->d_inode NULL and instantiate inode pointer later
 */
static inline void fsnotify_link(struct inode *inode, struct dentry *new_dentry)
{
	fsnotify_link_count(inode);
	audit_dentry_child(new_dentry, AUDIT_TYPE_CHILD_CREATE);

	fsnotify_d_name(new_dentry, FS_CREATE);
}

/*
 * fsnotify_mkdir - directory 'name' was created
 */
static inline void fsnotify_mkdir(struct dentry *dentry)
{
	__u32 mask = (FS_CREATE | FS_ISDIR);

	audit_dentry_child(dentry, AUDIT_TYPE_CHILD_CREATE);

	fsnotify_d_name(dentry, mask);
}

/*
 * fsnotify_access - file was read
 */
static inline void fsnotify_access(struct file *file)
{
	struct path *path = &file->f_path;
	struct inode *inode = path->dentry->d_inode;
	__u32 mask = FS_ACCESS;

	if (S_ISDIR(inode->i_mode))
		mask |= FS_ISDIR;

	if (!(file->f_mode & FMODE_NONOTIFY)) {
		fsnotify_parent(path, NULL, mask);
		fsnotify(inode, mask, path, FSNOTIFY_EVENT_PATH, NULL, 0);
	}
}

/*
 * fsnotify_modify - file was modified
 */
static inline void fsnotify_modify(struct file *file)
{
	struct path *path = &file->f_path;
	struct inode *inode = path->dentry->d_inode;
	__u32 mask = FS_MODIFY;

	if (S_ISDIR(inode->i_mode))
		mask |= FS_ISDIR;

	if (!(file->f_mode & FMODE_NONOTIFY)) {
		fsnotify_parent(path, NULL, mask);
		fsnotify(inode, mask, path, FSNOTIFY_EVENT_PATH, NULL, 0);
	}
}

/*
 * fsnotify_open - file was opened
 */
static inline void fsnotify_open(struct file *file)
{
	struct path *path = &file->f_path;
	struct inode *inode = path->dentry->d_inode;
	__u32 mask = FS_OPEN;

	if (S_ISDIR(inode->i_mode))
		mask |= FS_ISDIR;

	fsnotify_parent(path, NULL, mask);
	fsnotify(inode, mask, path, FSNOTIFY_EVENT_PATH, NULL, 0);
}

/*
 * fsnotify_close - file was closed
 */
static inline void fsnotify_close(struct file *file)
{
	struct path *path = &file->f_path;
	struct inode *inode = path->dentry->d_inode;
	fmode_t mode = file->f_mode;
	__u32 mask = (mode & FMODE_WRITE) ? FS_CLOSE_WRITE : FS_CLOSE_NOWRITE;

	if (S_ISDIR(inode->i_mode))
		mask |= FS_ISDIR;

	if (!(file->f_mode & FMODE_NONOTIFY)) {
		fsnotify_parent(path, NULL, mask);
		fsnotify(inode, mask, path, FSNOTIFY_EVENT_PATH, NULL, 0);
	}
}

/*
 * fsnotify_xattr - extended attributes were changed
 */
static inline void fsnotify_xattr(struct dentry *dentry)
{
	struct inode *inode = dentry->d_inode;
	__u32 mask = FS_ATTRIB;

	if (S_ISDIR(inode->i_mode))
		mask |= FS_ISDIR;

	fsnotify_parent(NULL, dentry, mask);
	fsnotify(inode, mask, dentry, FSNOTIFY_EVENT_DENTRY, NULL, 0);
}

/*
 * fsnotify_change - notify_change event.  file was modified and/or metadata
 * was changed.
 */
static inline void fsnotify_change(struct dentry *dentry, unsigned int ia_valid)
{
	struct inode *inode = dentry->d_inode;
	__u32 mask = 0;

	if (ia_valid & ATTR_UID)
		mask |= FS_ATTRIB;
	if (ia_valid & ATTR_GID)
		mask |= FS_ATTRIB;
	if (ia_valid & ATTR_SIZE)
		mask |= FS_MODIFY;

	/* both times implies a utime(s) call */
	if ((ia_valid & (ATTR_ATIME | ATTR_MTIME)) == (ATTR_ATIME | ATTR_MTIME))
		mask |= FS_ATTRIB;
	else if (ia_valid & ATTR_ATIME)
		mask |= FS_ACCESS;
	else if (ia_valid & ATTR_MTIME)
		mask |= FS_MODIFY;

	if (ia_valid & ATTR_MODE)
		mask |= FS_ATTRIB;

	if (mask) {
		if (S_ISDIR(inode->i_mode))
			mask |= FS_ISDIR;

		fsnotify_parent(NULL, dentry, mask);
		fsnotify(inode, mask, dentry, FSNOTIFY_EVENT_DENTRY, NULL, 0);
	}
}

#endif	/* _LINUX_FS_NOTIFY_H */
