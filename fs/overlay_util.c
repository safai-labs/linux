/*
 * Copyright (C) 2017 Red Hat, Inc.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 as published by
 * the Free Software Foundation.
 */
#if IS_ENABLED(CONFIG_OVERLAY_FS)

#include <linux/overlay_util.h>
#include <linux/fs.h>
#include <linux/file.h>
#include <linux/mm.h>
#include "internal.h"

static bool overlay_file_consistent(struct file *file)
{
	return d_real_inode(file->f_path.dentry) == file_inode(file);
}

ssize_t overlay_read_iter(struct file *file, struct kiocb *kio,
			  struct iov_iter *iter)
{
	ssize_t ret;

	if (likely(overlay_file_consistent(file)))
		return file->f_op->read_iter(kio, iter);

	file = filp_clone_open(file);
	if (IS_ERR(file))
		return PTR_ERR(file);

	ret = vfs_iter_read(file, iter, &kio->ki_pos);
	fput(file);

	return ret;
}
EXPORT_SYMBOL(overlay_read_iter);

int overlay_fsync(struct file *file, loff_t start, loff_t end,
		  int datasync)
{
	int ret;

	if (likely(overlay_file_consistent(file)))
		return file->f_op->fsync(file, start, end, datasync);

	file = filp_clone_open(file);
	if (IS_ERR(file))
		return PTR_ERR(file);

	ret = vfs_fsync_range(file, start, end, datasync);
	fput(file);

	return ret;
}
EXPORT_SYMBOL(overlay_fsync);

int overlay_mmap(struct file *file, struct vm_area_struct *vma)
{
	if (unlikely(!overlay_file_consistent(file))) {
		file = filp_clone_open(file);
		if (IS_ERR(file))
			return PTR_ERR(file);

		fput(vma->vm_file);
		/* transfer ref: */
		vma->vm_file = file;

		if (!file->f_op->mmap)
			return -ENODEV;
	}
	return file->f_op->mmap(file, vma);
}
EXPORT_SYMBOL(overlay_mmap);

#endif /* IS_ENABLED(CONFIG_OVERLAY_FS) */
