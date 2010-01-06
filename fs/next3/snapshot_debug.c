/*
 * linux/fs/next3/snapshot_debug.c
 *
 * Copyright (C) 2008 Amir Goldor <amir73il@users.sf.net>
 *
 * This file is part of the Linux kernel and is made available under
 * the terms of the GNU General Public License, version 2, or at your
 * option, any later version, incorporated herein by reference.
 *
 * Next3 snapshot debugging.
 */


#include <linux/module.h>
#include <linux/proc_fs.h>
#include <linux/debugfs.h>
#include "snapshot.h"

/*
 * debugfs tunables
 */

const char *snapshot_ident = SNAPSHOT_IDENT_STR + SNAPSHOT_IDENT_MAX;

static const char *snapshot_test_names[SNAPSHOT_TESTS_NUM] = {
	"test-take-delay-msec",
	"test-delete-delay-msec",
	"test-cow-delay-msec",
	"test-read-delay-msec",
	"test-bitmap-delay-msec",
};

#define SNAPSHOT_TEST_NAMES (sizeof(snapshot_test_names)/sizeof(snapshot_test_names[0]))

u16 snapshot_enable_test[SNAPSHOT_TESTS_NUM] __read_mostly = {0};
u8 snapshot_enable_debug __read_mostly = 1;

EXPORT_SYMBOL(snapshot_enable_test);
EXPORT_SYMBOL(snapshot_enable_debug);

static struct dentry *snapshot_debugfs_dir;
static struct dentry *snapshot_debug;
static struct dentry *snapshot_version;
static struct dentry *snapshot_test[SNAPSHOT_TESTS_NUM];

static char snapshot_version_str[] = NEXT3_SNAPSHOT_VERSION;
struct debugfs_blob_wrapper snapshot_version_blob = {
	.data = snapshot_version_str,
	.size = sizeof(snapshot_version_str)
};

static void snapshot_create_debugfs_entry(void)
{
	int i;
	snapshot_debugfs_dir = debugfs_create_dir("snapshot", NULL);
	if (snapshot_debugfs_dir) {
		snapshot_debug = debugfs_create_u8("snapshot-debug", S_IRUGO|S_IWUSR,
					       snapshot_debugfs_dir,
					       &snapshot_enable_debug);
		snapshot_version = debugfs_create_blob("snapshot-version", S_IRUGO,
					       snapshot_debugfs_dir,
					       &snapshot_version_blob);
		for (i=0; i<SNAPSHOT_TESTS_NUM && i<SNAPSHOT_TEST_NAMES; i++)
			snapshot_test[i] = debugfs_create_u16(snapshot_test_names[i], S_IRUGO|S_IWUSR,
					snapshot_debugfs_dir,
					&snapshot_enable_test[i]);
	}
}

static void snapshot_remove_debugfs_entry(void)
{
	int i;
	for (i=0; i<SNAPSHOT_TESTS_NUM && i<SNAPSHOT_TEST_NAMES; i++)
		debugfs_remove(snapshot_test[i]);
	debugfs_remove(snapshot_version);
	debugfs_remove(snapshot_debug);
	debugfs_remove(snapshot_debugfs_dir);
}

const char *snapshot_cmd_str(int cmd)
{
	switch (cmd) {
	case SNAPSHOT_READ:
		return "read";
	case SNAPSHOT_WRITE:
		return "write";
	case SNAPSHOT_COPY:
		return "copy";
	case SNAPSHOT_MOVE:
		return "move";
	case SNAPSHOT_CLEAR:
		return "clear";
	default:
		return "unknown";
	}
}

const char *snapshot_ret_str(int ret)
{
	return ret ? "in-use" : "not in-use";
}

/*
 * next snapshot module ctor/dtor
 */
int init_next3_snapshot(void)
{
	snapshot_create_debugfs_entry();
	return 0;
}

void exit_next3_snapshot(void)
{
	snapshot_remove_debugfs_entry();
}


