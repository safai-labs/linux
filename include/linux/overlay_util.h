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

#endif /* _LINUX_OVERLAY_FS_H */
