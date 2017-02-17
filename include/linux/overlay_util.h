#ifndef _LINUX_OVERLAY_FS_H
#define _LINUX_OVERLAY_FS_H

#include <linux/types.h>

struct file;
struct kiocb;
struct iov_iter;
struct vm_area_struct;

extern ssize_t overlay_read_iter(struct file *file, struct kiocb *kio,
				 struct iov_iter *iter);
extern int overlay_mmap(struct file *file, struct vm_area_struct *vma);
extern int overlay_fsync(struct file *file, loff_t start, loff_t end,
			 int datasync);

#endif /* _LINUX_OVERLAY_FS_H */
