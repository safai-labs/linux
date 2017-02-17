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

#endif /* IS_ENABLED(CONFIG_OVERLAY_FS) */
