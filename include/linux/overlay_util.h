#ifndef _LINUX_OVERLAY_FS_H
#define _LINUX_OVERLAY_FS_H

#include <linux/types.h>

struct file;
struct kiocb;
struct iov_iter;

extern ssize_t overlay_read_iter(struct file *file, struct kiocb *kio,
				 struct iov_iter *iter);

#endif /* _LINUX_OVERLAY_FS_H */
