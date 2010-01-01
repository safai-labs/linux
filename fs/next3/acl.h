/*
  File: fs/next3/acl.h

  (C) 2001 Andreas Gruenbacher, <a.gruenbacher@computer.org>
*/

#include <linux/posix_acl_xattr.h>

#define NEXT3_ACL_VERSION	0x0001

typedef struct {
	__le16		e_tag;
	__le16		e_perm;
	__le32		e_id;
} next3_acl_entry;

typedef struct {
	__le16		e_tag;
	__le16		e_perm;
} next3_acl_entry_short;

typedef struct {
	__le32		a_version;
} next3_acl_header;

static inline size_t next3_acl_size(int count)
{
	if (count <= 4) {
		return sizeof(next3_acl_header) +
		       count * sizeof(next3_acl_entry_short);
	} else {
		return sizeof(next3_acl_header) +
		       4 * sizeof(next3_acl_entry_short) +
		       (count - 4) * sizeof(next3_acl_entry);
	}
}

static inline int next3_acl_count(size_t size)
{
	ssize_t s;
	size -= sizeof(next3_acl_header);
	s = size - 4 * sizeof(next3_acl_entry_short);
	if (s < 0) {
		if (size % sizeof(next3_acl_entry_short))
			return -1;
		return size / sizeof(next3_acl_entry_short);
	} else {
		if (s % sizeof(next3_acl_entry))
			return -1;
		return s / sizeof(next3_acl_entry) + 4;
	}
}

#ifdef CONFIG_NEXT3_FS_POSIX_ACL

/* acl.c */
extern int next3_permission (struct inode *, int);
extern int next3_acl_chmod (struct inode *);
extern int next3_init_acl (handle_t *, struct inode *, struct inode *);

#else  /* CONFIG_NEXT3_FS_POSIX_ACL */
#include <linux/sched.h>
#define next3_permission NULL

static inline int
next3_acl_chmod(struct inode *inode)
{
	return 0;
}

static inline int
next3_init_acl(handle_t *handle, struct inode *inode, struct inode *dir)
{
	return 0;
}
#endif  /* CONFIG_NEXT3_FS_POSIX_ACL */

