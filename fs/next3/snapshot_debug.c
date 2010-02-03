/*
 * linux/fs/next3/snapshot_debug.c
 *
 * Written by Amir Goldstein <amir@ctera.com>, 2008
 *
 * Copyright (C) 2008-2010 CTERA Networks
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

const char *snapshot_indent = SNAPSHOT_INDENT_STR + SNAPSHOT_INDENT_MAX;

/*
 * Tunable delay values per snapshot operation for testing of
 * COW race conditions and master snapshot_mutex lock
 */
static const char *snapshot_test_names[SNAPSHOT_TESTS_NUM] = {
	/* delay completion of snapshot create|take */
	"test-take-delay-msec",
	/* delay completion of snapshot shrink|cleanup */
	"test-delete-delay-msec",
	/* delay completion of COW operation */
	"test-cow-delay-msec",
	/* delay submission of tracked read */
	"test-read-delay-msec",
	/* delay completion of COW bitmap operation */
	"test-bitmap-delay-msec",
};

#define SNAPSHOT_TEST_NAMES (sizeof(snapshot_test_names) / \
			     sizeof(snapshot_test_names[0]))

u16 snapshot_enable_test[SNAPSHOT_TESTS_NUM] __read_mostly = {0};
u8 snapshot_enable_debug __read_mostly = 1;

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
	if (!snapshot_debugfs_dir)
		return;
	snapshot_debug = debugfs_create_u8("snapshot-debug", S_IRUGO|S_IWUSR,
					   snapshot_debugfs_dir,
					   &snapshot_enable_debug);
	snapshot_version = debugfs_create_blob("snapshot-version", S_IRUGO,
					       snapshot_debugfs_dir,
					       &snapshot_version_blob);
	for (i = 0; i < SNAPSHOT_TESTS_NUM && i < SNAPSHOT_TEST_NAMES; i++)
		snapshot_test[i] = debugfs_create_u16(snapshot_test_names[i],
					      S_IRUGO|S_IWUSR,
					      snapshot_debugfs_dir,
					      &snapshot_enable_test[i]);
}

static void snapshot_remove_debugfs_entry(void)
{
	int i;

	for (i = 0; i < SNAPSHOT_TESTS_NUM && i < SNAPSHOT_TEST_NAMES; i++)
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

/*
 * Dump snapshot inode blocks map
 *
 * Use stack of indirect block iterators to traverse the snapshot inode (DFS)
 * and printk(KERN_DEBUG) a snapshot inode blocks map.
 *
 * Sample output:
 * snapshot (4) block map:
 * dind[0] = [30720/35]
 * {
 * 	ind[0] = [30722/35]
 * 	{
 * 		block[0-1/0] = [30723-30724/35]
 * 		block[129-131/0] = [30725-30727/35]
 * 	}
 * }
 * tind[0] = [30721/35]
 * {
 * 	dind[1] = [30728/35]
 * 	{
 * 		ind[1120] = [30729/35]
 * 		{
 * 			block[0-2/35] = [30730-30732/35]
 * 		}
 * 		ind[1124] = [30733/35]
 * 		{
 * 			block[4097/35] = [30734/35]
 * 			block[4103/35]
 * 			block[4108/35]
 * 		}
 * 	}
 * }
 * snapshot (4) contains: 0 (meta) + 6 (indirect) + 11 (data) = 17 blocks = 68K = 0M
 * snapshot (4) maps: 9 (copied) + 2 (moved) = 11 blocks
 */

/* next3 indirect block iterator */
struct next3_ind {
	__le32	*p;	/* cursor to mapped blocks array */
	u32	key;	/* address of current mapped block */
	struct buffer_head *bh; /* data of current mapped block */
};

/* snapshot dump state */
struct next3_dump_info { 
	struct inode *di_inode; /* snapshot inode */
	int nmeta;	/* no. of meta blocks */
	int nind;	/* no. of ind blocks */
	int ncopied;	/* no. of copied data blocks */
	int nmoved;	/* no. of moved data blocks */
};

/*
 * next3_snapshot_dump_ind - dump indirect block
 *	@di:	snapshot dump state
 *	@nr:	address of indirect block
 *	@idx:	index of indirect block
 *	@l:	level of indentation for debug prints
 *
 * Sample output:
 * 		ind[1120] = [30729/35]
 * 		{
 * 			block[0-2/35] = [30730-30732/35]
 * 		}
 */
static int next3_snapshot_dump_ind(struct next3_dump_info *di,
		u32 nr, int idx, int l)
{
	/* buffer of data blocks array */
	struct buffer_head *bh = NULL;
	/* cursor to data blocks array */
	__le32 *p;
	/* prev and curr data block address */
	u32 prev_key, key = 0;
	/* logical snapshot block (inode offset) */
	u32 blk = idx << SNAPSHOT_ADDR_PER_BLOCK_BITS;
	/* logical snapshot block group/start */
	u32 b0 = blk - SNAPSHOT_BLOCK_GROUP_OFFSET(blk);
	u32 grp = SNAPSHOT_BLOCK_GROUP(blk);
	int i, k = 0;

	snapshot_debug_l(5, l, "ind[%d] = [%u/%u]\n", idx,
			SNAPSHOT_BLOCK_GROUP_OFFSET(nr),
			SNAPSHOT_BLOCK_GROUP(nr));
	di->nind++;

	bh = sb_bread(di->di_inode->i_sb, nr);
	if (!bh)
		return -EIO;

	snapshot_debug_l(5, l, "{\n");
	p = (__le32 *)bh->b_data;
	/* itertate on data blocks array */
	for (i = 0; i <= SNAPSHOT_ADDR_PER_BLOCK; i++, p++, blk++) {
		prev_key = key;
		if (i < SNAPSHOT_ADDR_PER_BLOCK)
			/* read curr mapped block address */
			key = le32_to_cpu(*p);
		else
			/* terminate mapped blocks array */
			key = 0;

		if (!prev_key)
			/* skip unmapped blocks */
			continue;
		if (key == prev_key+1) {
			/* count subsequent mapped blocks */
			k++;
			continue;
		}
		
		if (k == 0) {
			/* (blk-1) is a group of 1 */
			if (prev_key == blk - 1) {
				/* print moved block */
				snapshot_debug_l(5, l+1,
					"block[%u/%u]\n",
					blk-1-b0, grp);
				di->nmoved++;
			} else {
				/* print copied block */
				snapshot_debug_l(5, l+1, "block[%u/%u]"
					" = [%u/%u]\n",
					blk-1-b0, grp,
					SNAPSHOT_BLOCK_GROUP_OFFSET(prev_key),
					SNAPSHOT_BLOCK_GROUP(prev_key));
				di->ncopied++;
			}
			continue;
		}

		/* (blk-1)-k..(blk-1) is a group of k+1 subsequent blocks */
		if (prev_key == blk - 1) {
			/* print group of subsequent moved blocks */
			snapshot_debug_l(5, l+1,
				"block[%u-%u/%u]\n",
				blk-1-k-b0, blk-1-b0, grp);
			di->nmoved += k+1;
		} else {
			/* print group of subsequent copied blocks */
			snapshot_debug_l(5, l+1, "block[%u-%u/%u]"
				" = [%u-%u/%u]\n",
				blk-1-k-b0, blk-1-b0, grp,
				SNAPSHOT_BLOCK_GROUP_OFFSET(prev_key)-k,
				SNAPSHOT_BLOCK_GROUP_OFFSET(prev_key),
				SNAPSHOT_BLOCK_GROUP(prev_key));
			di->ncopied += k+1;
		}
		/* reset subsequent blocks count */
		k = 0;
	}
	snapshot_debug_l(5, l, "}\n");
	brelse(bh);
	return 0;
}

/*
 * next3_snapshot_dump_dind - dump double indirect block
 *	@di:	snapshot dump state
 *	@nr:	address of double indirect block
 *	@idx:	index of double indirect block
 *	@l:	level of indentation for debug prints
 *
 * Sample output:
 * 	dind[1] = [30728/35]
 * 	{
 * 		ind[1120] = [30729/35]
 * 		{
 * 			block[0-2/35] = [30730-30732/35]
 * 		}
 * 		...
 */
static int next3_snapshot_dump_dind(struct next3_dump_info *di,
		u32 nr, int idx, int l)
{
	/* buffer of indirect blocks array */
	struct buffer_head *bh = NULL;
	/* cursor to indirect blocks array */
	__le32 *p;
	int i, err = 0;

	snapshot_debug_l(5, l, "dind[%d] = [%u/%u]\n", idx,
			SNAPSHOT_BLOCK_GROUP_OFFSET(nr),
			SNAPSHOT_BLOCK_GROUP(nr));
	di->nind++;

	bh = sb_bread(di->di_inode->i_sb, nr);
	if (!bh)
		return -EIO;

	snapshot_debug_l(5, l, "{\n");
	p = (__le32 *)bh->b_data;
	for (i = 0; i < SNAPSHOT_ADDR_PER_BLOCK; i++, p++) {
		if (!*p)
			continue;
		err = next3_snapshot_dump_ind(di, le32_to_cpu(*p),
				(idx << SNAPSHOT_ADDR_PER_BLOCK_BITS) + i,
				l+1);
		if (err)
			break;
	}
	snapshot_debug_l(5, l, "}\n");
	brelse(bh);
	return err;
}

/*
 * next3_snapshot_dump - print a snapshot @inode block map
 * Called from snapshot_load() on mount time under sb_lock
 * Called from snapshot_set_flags() under i_mutex
 */
void next3_snapshot_dump(struct inode *inode)
{
	/* indirect blocks stack/pointer */
	struct next3_ind ind_stack[4];
	struct next3_ind *ind = ind_stack;
	struct next3_dump_info di;
	struct next3_inode_info *ei = NEXT3_I(inode);
	int nblocks, i, n = 0, l = 0;

	memset(ind_stack, 0, sizeof(ind_stack));
	memset(&di, 0, sizeof(di));
	di.di_inode = inode;

	snapshot_debug(5, "snapshot (%u) block map:\n", inode->i_generation);
	/* print direct (meta) blocks */
	for (i = 0; i < NEXT3_NDIR_BLOCKS; i++) {
		if (ei->i_data[i]) {
			ind->key = le32_to_cpu(ei->i_data[i]);
			snapshot_debug_l(5, l, "meta[%d] = [%u/%u]\n", i,
					SNAPSHOT_BLOCK_GROUP_OFFSET(ind->key),
					SNAPSHOT_BLOCK_GROUP(ind->key));
			di.nmeta++;
		}
	}
	ind->key = le32_to_cpu(ei->i_data[NEXT3_DIND_BLOCK]);
	if (!ind->key)
		goto ind_out;
dump_dind:
	if (next3_snapshot_dump_dind(&di, ind->key, n, l))
		goto ind_out;

	if (ind == ind_stack) {
		/* print triple indirect map */
		ind->key = le32_to_cpu(ei->i_data[NEXT3_TIND_BLOCK]);
		if (!ind->key)
			goto ind_out;
		brelse(ind->bh);
		ind->bh = sb_bread(inode->i_sb, ind->key);
		if (!ind->bh)
			goto ind_out;
		snapshot_debug_l(5, l, "tind[0] = [%u/%u]\n",
				SNAPSHOT_BLOCK_GROUP_OFFSET(ind->key),
				SNAPSHOT_BLOCK_GROUP(ind->key));
#warning u can swap next two lines and change next one to ind->p = ...
		(ind+1)->p = (__le32 *)ind->bh->b_data;
		ind++;
		di.nind++;
		snapshot_debug_l(5, l, "{\n");
		l++;
	}

	if (ind > ind_stack) {
#warning i think u can safely eliminate the "n" variable and reuse "i" below.
		while (n++ < SNAPSHOT_ADDR_PER_BLOCK) {
			ind->key = le32_to_cpu(*(ind->p++));
			if (ind->key)
				goto dump_dind;
		}
	}

	l--;
	snapshot_debug_l(5, l, "}\n");
	ind--;
ind_out:
	nblocks = di.nmeta + di.nind + di.ncopied + di.nmoved;
	snapshot_debug(5, "snapshot (%u) contains: %d (meta) + %d (indirect) "
		       "+ %d (data) = %d blocks = %dK = %dM\n",
		       inode->i_generation, di.nmeta, di.nind, di.ncopied + di.nmoved,
		       nblocks, nblocks << (SNAPSHOT_BLOCK_SIZE_BITS - 10),
		       nblocks >> (20 - SNAPSHOT_BLOCK_SIZE_BITS));
	snapshot_debug(5, "snapshot (%u) maps: %d (copied) + %d (moved) = "
		       "%d blocks\n",
		       inode->i_generation, di.ncopied, di.nmoved, di.ncopied + di.nmoved);
	while (ind > ind_stack) {
		ind--;
		brelse(ind->bh);
	}
}
