/*
 *  linux/fs/next3/resize.c
 *
 * Support for resizing an next3 filesystem while it is mounted.
 *
 * Copyright (C) 2001, 2002 Andreas Dilger <adilger@clusterfs.com>
 *
 * This could probably be made into a module, because it is not often in use.
 */


#define NEXT3FS_DEBUG

#include "next3_jbd.h"
#include "snapshot.h"

#include <linux/errno.h>
#include <linux/slab.h>


#define outside(b, first, last)	((b) < (first) || (b) >= (last))
#define inside(b, first, last)	((b) >= (first) && (b) < (last))

static int verify_group_input(struct super_block *sb,
			      struct next3_new_group_data *input)
{
	struct next3_sb_info *sbi = NEXT3_SB(sb);
	struct next3_super_block *es = sbi->s_es;
	next3_fsblk_t start = le32_to_cpu(es->s_blocks_count);
	next3_fsblk_t end = start + input->blocks_count;
	unsigned group = input->group;
	next3_fsblk_t itend = input->inode_table + sbi->s_itb_per_group;
	unsigned overhead = next3_bg_has_super(sb, group) ?
		(1 + next3_bg_num_gdb(sb, group) +
		 le16_to_cpu(es->s_reserved_gdt_blocks)) : 0;
	next3_fsblk_t metaend = start + overhead;
	struct buffer_head *bh = NULL;
	next3_grpblk_t free_blocks_count;
	int err = -EINVAL;

	input->free_blocks_count = free_blocks_count =
		input->blocks_count - 2 - overhead - sbi->s_itb_per_group;

#ifdef CONFIG_NEXT3_FS_SNAPSHOT_EXCLUDE_INODE
	if (NEXT3_HAS_COMPAT_FEATURE(sb,
		NEXT3_FEATURE_COMPAT_EXCLUDE_INODE)) {
		/* reserve first free block for exclude bitmap */
		itend++;
		free_blocks_count--;
		input->free_blocks_count = free_blocks_count;
	}
#endif

	if (test_opt(sb, DEBUG))
		printk(KERN_DEBUG "NEXT3-fs: adding %s group %u: %u blocks "
		       "(%d free, %u reserved)\n",
		       next3_bg_has_super(sb, input->group) ? "normal" :
		       "no-super", input->group, input->blocks_count,
		       free_blocks_count, input->reserved_blocks);

	if (group != sbi->s_groups_count)
		next3_warning(sb, __func__,
			     "Cannot add at group %u (only %lu groups)",
			     input->group, sbi->s_groups_count);
	else if ((start - le32_to_cpu(es->s_first_data_block)) %
		 NEXT3_BLOCKS_PER_GROUP(sb))
		next3_warning(sb, __func__, "Last group not full");
	else if (input->reserved_blocks > input->blocks_count / 5)
		next3_warning(sb, __func__, "Reserved blocks too high (%u)",
			     input->reserved_blocks);
	else if (free_blocks_count < 0)
		next3_warning(sb, __func__, "Bad blocks count %u",
			     input->blocks_count);
	else if (!(bh = sb_bread(sb, end - 1)))
		next3_warning(sb, __func__,
			     "Cannot read last block ("E3FSBLK")",
			     end - 1);
	else if (outside(input->block_bitmap, start, end))
		next3_warning(sb, __func__,
			     "Block bitmap not in group (block %u)",
			     input->block_bitmap);
	else if (outside(input->inode_bitmap, start, end))
		next3_warning(sb, __func__,
			     "Inode bitmap not in group (block %u)",
			     input->inode_bitmap);
	else if (outside(input->inode_table, start, end) ||
	         outside(itend - 1, start, end))
		next3_warning(sb, __func__,
			     "Inode table not in group (blocks %u-"E3FSBLK")",
			     input->inode_table, itend - 1);
	else if (input->inode_bitmap == input->block_bitmap)
		next3_warning(sb, __func__,
			     "Block bitmap same as inode bitmap (%u)",
			     input->block_bitmap);
	else if (inside(input->block_bitmap, input->inode_table, itend))
		next3_warning(sb, __func__,
			     "Block bitmap (%u) in inode table (%u-"E3FSBLK")",
			     input->block_bitmap, input->inode_table, itend-1);
	else if (inside(input->inode_bitmap, input->inode_table, itend))
		next3_warning(sb, __func__,
			     "Inode bitmap (%u) in inode table (%u-"E3FSBLK")",
			     input->inode_bitmap, input->inode_table, itend-1);
	else if (inside(input->block_bitmap, start, metaend))
		next3_warning(sb, __func__,
			     "Block bitmap (%u) in GDT table"
			     " ("E3FSBLK"-"E3FSBLK")",
			     input->block_bitmap, start, metaend - 1);
	else if (inside(input->inode_bitmap, start, metaend))
		next3_warning(sb, __func__,
			     "Inode bitmap (%u) in GDT table"
			     " ("E3FSBLK"-"E3FSBLK")",
			     input->inode_bitmap, start, metaend - 1);
	else if (inside(input->inode_table, start, metaend) ||
	         inside(itend - 1, start, metaend))
		next3_warning(sb, __func__,
			     "Inode table (%u-"E3FSBLK") overlaps"
			     "GDT table ("E3FSBLK"-"E3FSBLK")",
			     input->inode_table, itend - 1, start, metaend - 1);
	else
		err = 0;
	brelse(bh);

	return err;
}

static struct buffer_head *bclean(handle_t *handle, struct super_block *sb,
				  next3_fsblk_t blk)
{
	struct buffer_head *bh;
	int err;

	bh = sb_getblk(sb, blk);
	if (!bh)
		return ERR_PTR(-EIO);
	if ((err = next3_journal_get_write_access(handle, bh))) {
		brelse(bh);
		bh = ERR_PTR(err);
	} else {
		lock_buffer(bh);
		memset(bh->b_data, 0, sb->s_blocksize);
		set_buffer_uptodate(bh);
		unlock_buffer(bh);
	}

	return bh;
}

/*
 * To avoid calling the atomic setbit hundreds or thousands of times, we only
 * need to use it within a single byte (to ensure we get endianness right).
 * We can use memset for the rest of the bitmap as there are no other users.
 */
static void mark_bitmap_end(int start_bit, int end_bit, char *bitmap)
{
	int i;

	if (start_bit >= end_bit)
		return;

	next3_debug("mark end bits +%d through +%d used\n", start_bit, end_bit);
	for (i = start_bit; i < ((start_bit + 7) & ~7UL); i++)
		next3_set_bit(i, bitmap);
	if (i < end_bit)
		memset(bitmap + (i >> 3), 0xff, (end_bit - i) >> 3);
}

/*
 * If we have fewer than thresh credits, extend by NEXT3_MAX_TRANS_DATA.
 * If that fails, restart the transaction & regain write access for the
 * buffer head which is used for block_bitmap modifications.
 */
static int extend_or_restart_transaction(handle_t *handle, int thresh,
					 struct buffer_head *bh)
{
	int err;

#ifdef CONFIG_NEXT3_FS_SNAPSHOT_JOURNAL_CREDITS
	if (NEXT3_SNAPSHOT_HAS_TRANS_BLOCKS(handle, thresh))
#else
	if (handle->h_buffer_credits >= thresh)
#endif
		return 0;

	err = next3_journal_extend(handle, NEXT3_MAX_TRANS_DATA);
	if (err < 0)
		return err;
	if (err) {
		err = next3_journal_restart(handle, NEXT3_MAX_TRANS_DATA);
		if (err)
			return err;
		err = next3_journal_get_write_access(handle, bh);
		if (err)
			return err;
	}

	return 0;
}

/*
 * Set up the block and inode bitmaps, and the inode table for the new group.
 * This doesn't need to be part of the main transaction, since we are only
 * changing blocks outside the actual filesystem.  We still do journaling to
 * ensure the recovery is correct in case of a failure just after resize.
 * If any part of this fails, we simply abort the resize.
 */
static int setup_new_group_blocks(struct super_block *sb,
				  struct next3_new_group_data *input)
{
	struct next3_sb_info *sbi = NEXT3_SB(sb);
	next3_fsblk_t start = next3_group_first_block_no(sb, input->group);
	next3_fsblk_t itend = input->inode_table + sbi->s_itb_per_group;
	int reserved_gdb = next3_bg_has_super(sb, input->group) ?
		le16_to_cpu(sbi->s_es->s_reserved_gdt_blocks) : 0;
	unsigned long gdblocks = next3_bg_num_gdb(sb, input->group);
	struct buffer_head *bh;
	handle_t *handle;
	next3_fsblk_t block;
	next3_grpblk_t bit;
	int i;
	int err = 0, err2;

	/* This transaction may be extended/restarted along the way */
	handle = next3_journal_start_sb(sb, NEXT3_MAX_TRANS_DATA);

	if (IS_ERR(handle))
		return PTR_ERR(handle);

	lock_super(sb);
	if (input->group != sbi->s_groups_count) {
		err = -EBUSY;
		goto exit_journal;
	}

	if (IS_ERR(bh = bclean(handle, sb, input->block_bitmap))) {
		err = PTR_ERR(bh);
		goto exit_journal;
	}

	if (next3_bg_has_super(sb, input->group)) {
		next3_debug("mark backup superblock %#04lx (+0)\n", start);
		next3_set_bit(0, bh->b_data);
	}

	/* Copy all of the GDT blocks into the backup in this group */
	for (i = 0, bit = 1, block = start + 1;
	     i < gdblocks; i++, block++, bit++) {
		struct buffer_head *gdb;

		next3_debug("update backup group %#04lx (+%d)\n", block, bit);

		err = extend_or_restart_transaction(handle, 1, bh);
		if (err)
			goto exit_bh;

		gdb = sb_getblk(sb, block);
		if (!gdb) {
			err = -EIO;
			goto exit_bh;
		}
		if ((err = next3_journal_get_write_access(handle, gdb))) {
			brelse(gdb);
			goto exit_bh;
		}
		lock_buffer(gdb);
		memcpy(gdb->b_data, sbi->s_group_desc[i]->b_data, gdb->b_size);
		set_buffer_uptodate(gdb);
		unlock_buffer(gdb);
		next3_journal_dirty_metadata(handle, gdb);
		next3_set_bit(bit, bh->b_data);
		brelse(gdb);
	}

	/* Zero out all of the reserved backup group descriptor table blocks */
	for (i = 0, bit = gdblocks + 1, block = start + bit;
	     i < reserved_gdb; i++, block++, bit++) {
		struct buffer_head *gdb;

		next3_debug("clear reserved block %#04lx (+%d)\n", block, bit);

		err = extend_or_restart_transaction(handle, 1, bh);
		if (err)
			goto exit_bh;

		if (IS_ERR(gdb = bclean(handle, sb, block))) {
			err = PTR_ERR(bh);
			goto exit_bh;
		}
		next3_journal_dirty_metadata(handle, gdb);
		next3_set_bit(bit, bh->b_data);
		brelse(gdb);
	}
	next3_debug("mark block bitmap %#04x (+%ld)\n", input->block_bitmap,
		   input->block_bitmap - start);
	next3_set_bit(input->block_bitmap - start, bh->b_data);
	next3_debug("mark inode bitmap %#04x (+%ld)\n", input->inode_bitmap,
		   input->inode_bitmap - start);
	next3_set_bit(input->inode_bitmap - start, bh->b_data);

#ifdef CONFIG_NEXT3_FS_SNAPSHOT_EXCLUDE_INODE
	if (NEXT3_HAS_COMPAT_FEATURE(sb,
		NEXT3_FEATURE_COMPAT_EXCLUDE_INODE)) {
		/* clear reserved exclude bitmap block */
		itend++;
	}
#endif

	/* Zero out all of the inode table blocks */
	for (block = input->inode_table, bit = block - start;
	     block < itend; bit++, block++) {
		struct buffer_head *it;

		next3_debug("clear inode block %#04lx (+%d)\n", block, bit);

		err = extend_or_restart_transaction(handle, 1, bh);
		if (err)
			goto exit_bh;

		if (IS_ERR(it = bclean(handle, sb, block))) {
			err = PTR_ERR(it);
			goto exit_bh;
		}
		next3_journal_dirty_metadata(handle, it);
		brelse(it);
		next3_set_bit(bit, bh->b_data);
	}

	err = extend_or_restart_transaction(handle, 2, bh);
	if (err)
		goto exit_bh;

	mark_bitmap_end(input->blocks_count, NEXT3_BLOCKS_PER_GROUP(sb),
			bh->b_data);
	next3_journal_dirty_metadata(handle, bh);
	brelse(bh);

	/* Mark unused entries in inode bitmap used */
	next3_debug("clear inode bitmap %#04x (+%ld)\n",
		   input->inode_bitmap, input->inode_bitmap - start);
	if (IS_ERR(bh = bclean(handle, sb, input->inode_bitmap))) {
		err = PTR_ERR(bh);
		goto exit_journal;
	}

	mark_bitmap_end(NEXT3_INODES_PER_GROUP(sb), NEXT3_BLOCKS_PER_GROUP(sb),
			bh->b_data);
	next3_journal_dirty_metadata(handle, bh);
exit_bh:
	brelse(bh);

exit_journal:
	unlock_super(sb);
	if ((err2 = next3_journal_stop(handle)) && !err)
		err = err2;

	return err;
}

/*
 * Iterate through the groups which hold BACKUP superblock/GDT copies in an
 * next3 filesystem.  The counters should be initialized to 1, 5, and 7 before
 * calling this for the first time.  In a sparse filesystem it will be the
 * sequence of powers of 3, 5, and 7: 1, 3, 5, 7, 9, 25, 27, 49, 81, ...
 * For a non-sparse filesystem it will be every group: 1, 2, 3, 4, ...
 */
static unsigned next3_list_backups(struct super_block *sb, unsigned *three,
				  unsigned *five, unsigned *seven)
{
	unsigned *min = three;
	int mult = 3;
	unsigned ret;

	if (!NEXT3_HAS_RO_COMPAT_FEATURE(sb,
					NEXT3_FEATURE_RO_COMPAT_SPARSE_SUPER)) {
		ret = *min;
		*min += 1;
		return ret;
	}

	if (*five < *min) {
		min = five;
		mult = 5;
	}
	if (*seven < *min) {
		min = seven;
		mult = 7;
	}

	ret = *min;
	*min *= mult;

	return ret;
}

/*
 * Check that all of the backup GDT blocks are held in the primary GDT block.
 * It is assumed that they are stored in group order.  Returns the number of
 * groups in current filesystem that have BACKUPS, or -ve error code.
 */
static int verify_reserved_gdb(struct super_block *sb,
			       struct buffer_head *primary)
{
	const next3_fsblk_t blk = primary->b_blocknr;
	const unsigned long end = NEXT3_SB(sb)->s_groups_count;
	unsigned three = 1;
	unsigned five = 5;
	unsigned seven = 7;
	unsigned grp;
	__le32 *p = (__le32 *)primary->b_data;
	int gdbackups = 0;

	while ((grp = next3_list_backups(sb, &three, &five, &seven)) < end) {
		if (le32_to_cpu(*p++) != grp * NEXT3_BLOCKS_PER_GROUP(sb) + blk){
			next3_warning(sb, __func__,
				     "reserved GDT "E3FSBLK
				     " missing grp %d ("E3FSBLK")",
				     blk, grp,
				     grp * NEXT3_BLOCKS_PER_GROUP(sb) + blk);
			return -EINVAL;
		}
		if (++gdbackups > NEXT3_ADDR_PER_BLOCK(sb))
			return -EFBIG;
	}

	return gdbackups;
}

/*
 * Called when we need to bring a reserved group descriptor table block into
 * use from the resize inode.  The primary copy of the new GDT block currently
 * is an indirect block (under the double indirect block in the resize inode).
 * The new backup GDT blocks will be stored as leaf blocks in this indirect
 * block, in group order.  Even though we know all the block numbers we need,
 * we check to ensure that the resize inode has actually reserved these blocks.
 *
 * Don't need to update the block bitmaps because the blocks are still in use.
 *
 * We get all of the error cases out of the way, so that we are sure to not
 * fail once we start modifying the data on disk, because JBD has no rollback.
 */
static int add_new_gdb(handle_t *handle, struct inode *inode,
		       struct next3_new_group_data *input,
		       struct buffer_head **primary)
{
	struct super_block *sb = inode->i_sb;
	struct next3_super_block *es = NEXT3_SB(sb)->s_es;
	unsigned long gdb_num = input->group / NEXT3_DESC_PER_BLOCK(sb);
	next3_fsblk_t gdblock = NEXT3_SB(sb)->s_sbh->b_blocknr + 1 + gdb_num;
	struct buffer_head **o_group_desc, **n_group_desc;
	struct buffer_head *dind;
	int gdbackups;
	struct next3_iloc iloc;
	__le32 *data;
	int err;

	if (test_opt(sb, DEBUG))
		printk(KERN_DEBUG
		       "NEXT3-fs: next3_add_new_gdb: adding group block %lu\n",
		       gdb_num);

	/*
	 * If we are not using the primary superblock/GDT copy don't resize,
	 * because the user tools have no way of handling this.  Probably a
	 * bad time to do it anyways.
	 */
	if (NEXT3_SB(sb)->s_sbh->b_blocknr !=
	    le32_to_cpu(NEXT3_SB(sb)->s_es->s_first_data_block)) {
		next3_warning(sb, __func__,
			"won't resize using backup superblock at %llu",
			(unsigned long long)NEXT3_SB(sb)->s_sbh->b_blocknr);
		return -EPERM;
	}

	*primary = sb_bread(sb, gdblock);
	if (!*primary)
		return -EIO;

	if ((gdbackups = verify_reserved_gdb(sb, *primary)) < 0) {
		err = gdbackups;
		goto exit_bh;
	}

	data = NEXT3_I(inode)->i_data + NEXT3_DIND_BLOCK;
	dind = sb_bread(sb, le32_to_cpu(*data));
	if (!dind) {
		err = -EIO;
		goto exit_bh;
	}

	data = (__le32 *)dind->b_data;
	if (le32_to_cpu(data[gdb_num % NEXT3_ADDR_PER_BLOCK(sb)]) != gdblock) {
		next3_warning(sb, __func__,
			     "new group %u GDT block "E3FSBLK" not reserved",
			     input->group, gdblock);
		err = -EINVAL;
		goto exit_dind;
	}

	if ((err = next3_journal_get_write_access(handle, NEXT3_SB(sb)->s_sbh)))
		goto exit_dind;

	if ((err = next3_journal_get_write_access(handle, *primary)))
		goto exit_sbh;

	if ((err = next3_journal_get_write_access(handle, dind)))
		goto exit_primary;

	/* next3_reserve_inode_write() gets a reference on the iloc */
	if ((err = next3_reserve_inode_write(handle, inode, &iloc)))
		goto exit_dindj;

	n_group_desc = kmalloc((gdb_num + 1) * sizeof(struct buffer_head *),
			GFP_NOFS);
	if (!n_group_desc) {
		err = -ENOMEM;
		next3_warning (sb, __func__,
			      "not enough memory for %lu groups", gdb_num + 1);
		goto exit_inode;
	}

	/*
	 * Finally, we have all of the possible failures behind us...
	 *
	 * Remove new GDT block from inode double-indirect block and clear out
	 * the new GDT block for use (which also "frees" the backup GDT blocks
	 * from the reserved inode).  We don't need to change the bitmaps for
	 * these blocks, because they are marked as in-use from being in the
	 * reserved inode, and will become GDT blocks (primary and backup).
	 */
	data[gdb_num % NEXT3_ADDR_PER_BLOCK(sb)] = 0;
	next3_journal_dirty_metadata(handle, dind);
	brelse(dind);
	inode->i_blocks -= (gdbackups + 1) * sb->s_blocksize >> 9;
	next3_mark_iloc_dirty(handle, inode, &iloc);
	memset((*primary)->b_data, 0, sb->s_blocksize);
	next3_journal_dirty_metadata(handle, *primary);

	o_group_desc = NEXT3_SB(sb)->s_group_desc;
	memcpy(n_group_desc, o_group_desc,
	       NEXT3_SB(sb)->s_gdb_count * sizeof(struct buffer_head *));
	n_group_desc[gdb_num] = *primary;
	NEXT3_SB(sb)->s_group_desc = n_group_desc;
	NEXT3_SB(sb)->s_gdb_count++;
	kfree(o_group_desc);

	le16_add_cpu(&es->s_reserved_gdt_blocks, -1);
	next3_journal_dirty_metadata(handle, NEXT3_SB(sb)->s_sbh);

	return 0;

exit_inode:
	//next3_journal_release_buffer(handle, iloc.bh);
	brelse(iloc.bh);
exit_dindj:
	//next3_journal_release_buffer(handle, dind);
exit_primary:
	//next3_journal_release_buffer(handle, *primary);
exit_sbh:
	//next3_journal_release_buffer(handle, *primary);
exit_dind:
	brelse(dind);
exit_bh:
	brelse(*primary);

	next3_debug("leaving with error %d\n", err);
	return err;
}

/*
 * Called when we are adding a new group which has a backup copy of each of
 * the GDT blocks (i.e. sparse group) and there are reserved GDT blocks.
 * We need to add these reserved backup GDT blocks to the resize inode, so
 * that they are kept for future resizing and not allocated to files.
 *
 * Each reserved backup GDT block will go into a different indirect block.
 * The indirect blocks are actually the primary reserved GDT blocks,
 * so we know in advance what their block numbers are.  We only get the
 * double-indirect block to verify it is pointing to the primary reserved
 * GDT blocks so we don't overwrite a data block by accident.  The reserved
 * backup GDT blocks are stored in their reserved primary GDT block.
 */
static int reserve_backup_gdb(handle_t *handle, struct inode *inode,
			      struct next3_new_group_data *input)
{
	struct super_block *sb = inode->i_sb;
	int reserved_gdb =le16_to_cpu(NEXT3_SB(sb)->s_es->s_reserved_gdt_blocks);
	struct buffer_head **primary;
	struct buffer_head *dind;
	struct next3_iloc iloc;
	next3_fsblk_t blk;
	__le32 *data, *end;
	int gdbackups = 0;
	int res, i;
	int err;

	primary = kmalloc(reserved_gdb * sizeof(*primary), GFP_NOFS);
	if (!primary)
		return -ENOMEM;

	data = NEXT3_I(inode)->i_data + NEXT3_DIND_BLOCK;
	dind = sb_bread(sb, le32_to_cpu(*data));
	if (!dind) {
		err = -EIO;
		goto exit_free;
	}

	blk = NEXT3_SB(sb)->s_sbh->b_blocknr + 1 + NEXT3_SB(sb)->s_gdb_count;
	data = (__le32 *)dind->b_data + (NEXT3_SB(sb)->s_gdb_count %
					 NEXT3_ADDR_PER_BLOCK(sb));
	end = (__le32 *)dind->b_data + NEXT3_ADDR_PER_BLOCK(sb);

	/* Get each reserved primary GDT block and verify it holds backups */
	for (res = 0; res < reserved_gdb; res++, blk++) {
		if (le32_to_cpu(*data) != blk) {
			next3_warning(sb, __func__,
				     "reserved block "E3FSBLK
				     " not at offset %ld",
				     blk,
				     (long)(data - (__le32 *)dind->b_data));
			err = -EINVAL;
			goto exit_bh;
		}
		primary[res] = sb_bread(sb, blk);
		if (!primary[res]) {
			err = -EIO;
			goto exit_bh;
		}
		if ((gdbackups = verify_reserved_gdb(sb, primary[res])) < 0) {
			brelse(primary[res]);
			err = gdbackups;
			goto exit_bh;
		}
		if (++data >= end)
			data = (__le32 *)dind->b_data;
	}

	for (i = 0; i < reserved_gdb; i++) {
		if ((err = next3_journal_get_write_access(handle, primary[i]))) {
			/*
			int j;
			for (j = 0; j < i; j++)
				next3_journal_release_buffer(handle, primary[j]);
			 */
			goto exit_bh;
		}
	}

	if ((err = next3_reserve_inode_write(handle, inode, &iloc)))
		goto exit_bh;

	/*
	 * Finally we can add each of the reserved backup GDT blocks from
	 * the new group to its reserved primary GDT block.
	 */
	blk = input->group * NEXT3_BLOCKS_PER_GROUP(sb);
	for (i = 0; i < reserved_gdb; i++) {
		int err2;
		data = (__le32 *)primary[i]->b_data;
		/* printk("reserving backup %lu[%u] = %lu\n",
		       primary[i]->b_blocknr, gdbackups,
		       blk + primary[i]->b_blocknr); */
		data[gdbackups] = cpu_to_le32(blk + primary[i]->b_blocknr);
		err2 = next3_journal_dirty_metadata(handle, primary[i]);
		if (!err)
			err = err2;
	}
	inode->i_blocks += reserved_gdb * sb->s_blocksize >> 9;
	next3_mark_iloc_dirty(handle, inode, &iloc);

exit_bh:
	while (--res >= 0)
		brelse(primary[res]);
	brelse(dind);

exit_free:
	kfree(primary);

	return err;
}

/*
 * Update the backup copies of the next3 metadata.  These don't need to be part
 * of the main resize transaction, because e2fsck will re-write them if there
 * is a problem (basically only OOM will cause a problem).  However, we
 * _should_ update the backups if possible, in case the primary gets trashed
 * for some reason and we need to run e2fsck from a backup superblock.  The
 * important part is that the new block and inode counts are in the backup
 * superblocks, and the location of the new group metadata in the GDT backups.
 *
 * We do not need lock_super() for this, because these blocks are not
 * otherwise touched by the filesystem code when it is mounted.  We don't
 * need to worry about last changing from sbi->s_groups_count, because the
 * worst that can happen is that we do not copy the full number of backups
 * at this time.  The resize which changed s_groups_count will backup again.
 */
static void update_backups(struct super_block *sb,
			   int blk_off, char *data, int size)
{
	struct next3_sb_info *sbi = NEXT3_SB(sb);
	const unsigned long last = sbi->s_groups_count;
	const int bpg = NEXT3_BLOCKS_PER_GROUP(sb);
	unsigned three = 1;
	unsigned five = 5;
	unsigned seven = 7;
	unsigned group;
	int rest = sb->s_blocksize - size;
	handle_t *handle;
	int err = 0, err2;

	handle = next3_journal_start_sb(sb, NEXT3_MAX_TRANS_DATA);
	if (IS_ERR(handle)) {
		group = 1;
		err = PTR_ERR(handle);
		goto exit_err;
	}

	while ((group = next3_list_backups(sb, &three, &five, &seven)) < last) {
		struct buffer_head *bh;

		/* Out of journal space, and can't get more - abort - so sad */
		int buffer_credits = handle->h_buffer_credits;
#ifdef CONFIG_NEXT3_FS_SNAPSHOT_JOURNAL_CREDITS
		if (!NEXT3_SNAPSHOT_HAS_TRANS_BLOCKS(handle, 1))
			buffer_credits = 0;
#endif
		if (buffer_credits == 0 &&
		    next3_journal_extend(handle, NEXT3_MAX_TRANS_DATA) &&
		    (err = next3_journal_restart(handle, NEXT3_MAX_TRANS_DATA)))
			break;

#ifdef CONFIG_NEXT3_FS_SNAPSHOT_HOOKS_JBD
		if (next3_snapshot_get_active(sb))
			/*
			 * next3_snapshot_get_write_access() expects an uptodate buffer
			 * do it here to supress "non uptodate buffer" warning. -goldor
			 */
			bh = sb_bread(sb, group * bpg + blk_off);
		else
#endif
		bh = sb_getblk(sb, group * bpg + blk_off);
		if (!bh) {
			err = -EIO;
			break;
		}
		next3_debug("update metadata backup %#04lx\n",
			  (unsigned long)bh->b_blocknr);
		if ((err = next3_journal_get_write_access(handle, bh)))
			break;
		lock_buffer(bh);
		memcpy(bh->b_data, data, size);
		if (rest)
			memset(bh->b_data + size, 0, rest);
		set_buffer_uptodate(bh);
		unlock_buffer(bh);
		next3_journal_dirty_metadata(handle, bh);
		brelse(bh);
	}
	if ((err2 = next3_journal_stop(handle)) && !err)
		err = err2;

	/*
	 * Ugh! Need to have e2fsck write the backup copies.  It is too
	 * late to revert the resize, we shouldn't fail just because of
	 * the backup copies (they are only needed in case of corruption).
	 *
	 * However, if we got here we have a journal problem too, so we
	 * can't really start a transaction to mark the superblock.
	 * Chicken out and just set the flag on the hope it will be written
	 * to disk, and if not - we will simply wait until next fsck.
	 */
exit_err:
	if (err) {
		next3_warning(sb, __func__,
			     "can't update backup for group %d (err %d), "
			     "forcing fsck on next reboot", group, err);
		sbi->s_mount_state &= ~NEXT3_VALID_FS;
		sbi->s_es->s_state &= cpu_to_le16(~NEXT3_VALID_FS);
		mark_buffer_dirty(sbi->s_sbh);
	}
}

/* Add group descriptor data to an existing or new group descriptor block.
 * Ensure we handle all possible error conditions _before_ we start modifying
 * the filesystem, because we cannot abort the transaction and not have it
 * write the data to disk.
 *
 * If we are on a GDT block boundary, we need to get the reserved GDT block.
 * Otherwise, we may need to add backup GDT blocks for a sparse group.
 *
 * We only need to hold the superblock lock while we are actually adding
 * in the new group's counts to the superblock.  Prior to that we have
 * not really "added" the group at all.  We re-check that we are still
 * adding in the last group in case things have changed since verifying.
 */
int next3_group_add(struct super_block *sb, struct next3_new_group_data *input)
{
	struct next3_sb_info *sbi = NEXT3_SB(sb);
	struct next3_super_block *es = sbi->s_es;
	int reserved_gdb = next3_bg_has_super(sb, input->group) ?
		le16_to_cpu(es->s_reserved_gdt_blocks) : 0;
	struct buffer_head *primary = NULL;
	struct next3_group_desc *gdp;
	struct inode *inode = NULL;
#ifdef CONFIG_NEXT3_FS_SNAPSHOT_EXCLUDE_INODE
	struct inode *exclude_inode = NULL;
	struct buffer_head *exclude_bh = NULL;
	__le32 *exclude_bitmap = NULL;
	int credits;
#endif
	handle_t *handle;
	int gdb_off, gdb_num;
	int err, err2;

	gdb_num = input->group / NEXT3_DESC_PER_BLOCK(sb);
	gdb_off = input->group % NEXT3_DESC_PER_BLOCK(sb);

	if (gdb_off == 0 && !NEXT3_HAS_RO_COMPAT_FEATURE(sb,
					NEXT3_FEATURE_RO_COMPAT_SPARSE_SUPER)) {
		next3_warning(sb, __func__,
			     "Can't resize non-sparse filesystem further");
		return -EPERM;
	}

	if (le32_to_cpu(es->s_blocks_count) + input->blocks_count <
	    le32_to_cpu(es->s_blocks_count)) {
		next3_warning(sb, __func__, "blocks_count overflow\n");
		return -EINVAL;
	}

	if (le32_to_cpu(es->s_inodes_count) + NEXT3_INODES_PER_GROUP(sb) <
	    le32_to_cpu(es->s_inodes_count)) {
		next3_warning(sb, __func__, "inodes_count overflow\n");
		return -EINVAL;
	}

	if (reserved_gdb || gdb_off == 0) {
		if (!NEXT3_HAS_COMPAT_FEATURE(sb,
					     NEXT3_FEATURE_COMPAT_RESIZE_INODE)
		    || !le16_to_cpu(es->s_reserved_gdt_blocks)) {
			next3_warning(sb, __func__,
				     "No reserved GDT blocks, can't resize");
			return -EPERM;
		}
		inode = next3_iget(sb, NEXT3_RESIZE_INO);
		if (IS_ERR(inode)) {
			next3_warning(sb, __func__,
				     "Error opening resize inode");
			return PTR_ERR(inode);
		}
	}

#ifdef CONFIG_NEXT3_FS_SNAPSHOT_EXCLUDE_INODE
	if (NEXT3_HAS_COMPAT_FEATURE(sb,
		NEXT3_FEATURE_COMPAT_EXCLUDE_INODE)) {
		int dind_offset = input->group/SNAPSHOT_ADDR_PER_BLOCK;
		int ind_offset = input->group%SNAPSHOT_ADDR_PER_BLOCK;
		int err;

		exclude_inode = next3_iget(sb, NEXT3_EXCLUDE_INO);
		if (IS_ERR(exclude_inode)) {
			next3_warning(sb, __func__,
				     "Error opening exclude inode");
			return PTR_ERR(exclude_inode);
		}

		/* exclude bitmap blocks addresses are exposed on the IND branch */
		exclude_bh = next3_bread(NULL, exclude_inode, NEXT3_IND_BLOCK+dind_offset, 0, &err);
		if (!exclude_bh) {
			snapshot_debug(1, "failed to read exclude inode indirect[%d] block\n",
					dind_offset);
			return err ? err : -EIO;
		}
		exclude_bitmap = ((__le32 *)exclude_bh->b_data) + ind_offset;
	}
#endif

	if ((err = verify_group_input(sb, input)))
		goto exit_put;

	if ((err = setup_new_group_blocks(sb, input)))
		goto exit_put;

	/*
	 * We will always be modifying at least the superblock and a GDT
	 * block.  If we are adding a group past the last current GDT block,
	 * we will also modify the inode and the dindirect block.  If we
	 * are adding a group with superblock/GDT backups  we will also
	 * modify each of the reserved GDT dindirect blocks.
	 */
#ifdef CONFIG_NEXT3_FS_SNAPSHOT_EXCLUDE_INODE
	credits = next3_bg_has_super(sb, input->group) ? 
		3 + reserved_gdb : 4;
	if (exclude_bitmap && !*exclude_bitmap)
		/*
		 * we will also be modifying the exclude inode 
		 * and one of it's indirect blocks
		 */
		credits += 2;
	handle = next3_journal_start_sb(sb, credits);
#else
	handle = next3_journal_start_sb(sb,
				       next3_bg_has_super(sb, input->group) ?
				       3 + reserved_gdb : 4);
#endif
	if (IS_ERR(handle)) {
		err = PTR_ERR(handle);
		goto exit_put;
	}

	lock_super(sb);
	if (input->group != sbi->s_groups_count) {
		next3_warning(sb, __func__,
			     "multiple resizers run on filesystem!");
		err = -EBUSY;
		goto exit_journal;
	}

	if ((err = next3_journal_get_write_access(handle, sbi->s_sbh)))
		goto exit_journal;

	/*
	 * We will only either add reserved group blocks to a backup group
	 * or remove reserved blocks for the first group in a new group block.
	 * Doing both would be mean more complex code, and sane people don't
	 * use non-sparse filesystems anymore.  This is already checked above.
	 */
	if (gdb_off) {
		primary = sbi->s_group_desc[gdb_num];
		if ((err = next3_journal_get_write_access(handle, primary)))
			goto exit_journal;

		if (reserved_gdb && next3_bg_num_gdb(sb, input->group) &&
		    (err = reserve_backup_gdb(handle, inode, input)))
			goto exit_journal;
	} else if ((err = add_new_gdb(handle, inode, input, &primary)))
		goto exit_journal;

	/*
	 * OK, now we've set up the new group.  Time to make it active.
	 *
	 * Current kernels don't lock all allocations via lock_super(),
	 * so we have to be safe wrt. concurrent accesses the group
	 * data.  So we need to be careful to set all of the relevant
	 * group descriptor data etc. *before* we enable the group.
	 *
	 * The key field here is sbi->s_groups_count: as long as
	 * that retains its old value, nobody is going to access the new
	 * group.
	 *
	 * So first we update all the descriptor metadata for the new
	 * group; then we update the total disk blocks count; then we
	 * update the groups count to enable the group; then finally we
	 * update the free space counts so that the system can start
	 * using the new disk blocks.
	 */

	/* Update group descriptor block for new group */
	gdp = (struct next3_group_desc *)primary->b_data + gdb_off;

	gdp->bg_block_bitmap = cpu_to_le32(input->block_bitmap);
	gdp->bg_inode_bitmap = cpu_to_le32(input->inode_bitmap);
	gdp->bg_inode_table = cpu_to_le32(input->inode_table);
	gdp->bg_free_blocks_count = cpu_to_le16(input->free_blocks_count);
	gdp->bg_free_inodes_count = cpu_to_le16(NEXT3_INODES_PER_GROUP(sb));
#ifdef CONFIG_NEXT3_FS_SNAPSHOT_EXCLUDE_INODE
	if (exclude_bitmap) {
		if (*exclude_bitmap) {
			/* 
			 * offline resize from a bigger size filesystem may leave
			 * allocated exclude bitmap blocks of unused block groups 
			 * -goldor 
			 */
			snapshot_debug(2, "reusing old exclude bitmap #%d block (%u)\n",
					input->group, le32_to_cpu(*exclude_bitmap));
		}
		else {
			/* set exclude bitmap block to first free block */
			next3_fsblk_t first_free = input->inode_table + sbi->s_itb_per_group;
			struct next3_iloc iloc;

			if ((err = next3_journal_get_write_access(handle, exclude_bh)))
				goto exit_journal;
			if ((err = next3_reserve_inode_write(handle, exclude_inode, &iloc)))
				goto exit_journal;

			*exclude_bitmap = cpu_to_le32(first_free);
			snapshot_debug(2, "allocated new exclude bitmap #%d block ("E3FSBLK")\n",
					input->group, first_free);
			next3_journal_dirty_metadata(handle, exclude_bh);
			
			/* update exclude inode size and blocks */
			i_size_write(exclude_inode, SNAPSHOT_IBLOCK(input->group) << SNAPSHOT_BLOCK_SIZE_BITS);
			NEXT3_I(exclude_inode)->i_disksize = exclude_inode->i_size;
			exclude_inode->i_blocks += sb->s_blocksize >> 9;
			next3_mark_iloc_dirty(handle, exclude_inode, &iloc);
		}
		/* update exclude bitmap cache */
		gdp->bg_exclude_bitmap = *exclude_bitmap;
	}
#endif

	/*
	 * Make the new blocks and inodes valid next.  We do this before
	 * increasing the group count so that once the group is enabled,
	 * all of its blocks and inodes are already valid.
	 *
	 * We always allocate group-by-group, then block-by-block or
	 * inode-by-inode within a group, so enabling these
	 * blocks/inodes before the group is live won't actually let us
	 * allocate the new space yet.
	 */
	le32_add_cpu(&es->s_blocks_count, input->blocks_count);
	le32_add_cpu(&es->s_inodes_count, NEXT3_INODES_PER_GROUP(sb));

	/*
	 * We need to protect s_groups_count against other CPUs seeing
	 * inconsistent state in the superblock.
	 *
	 * The precise rules we use are:
	 *
	 * * Writers of s_groups_count *must* hold lock_super
	 * AND
	 * * Writers must perform a smp_wmb() after updating all dependent
	 *   data and before modifying the groups count
	 *
	 * * Readers must hold lock_super() over the access
	 * OR
	 * * Readers must perform an smp_rmb() after reading the groups count
	 *   and before reading any dependent data.
	 *
	 * NB. These rules can be relaxed when checking the group count
	 * while freeing data, as we can only allocate from a block
	 * group after serialising against the group count, and we can
	 * only then free after serialising in turn against that
	 * allocation.
	 */
	smp_wmb();

	/* Update the global fs size fields */
	sbi->s_groups_count++;

	next3_journal_dirty_metadata(handle, primary);

	/* Update the reserved block counts only once the new group is
	 * active. */
	le32_add_cpu(&es->s_r_blocks_count, input->reserved_blocks);

	/* Update the free space counts */
	percpu_counter_add(&sbi->s_freeblocks_counter,
			   input->free_blocks_count);
	percpu_counter_add(&sbi->s_freeinodes_counter,
			   NEXT3_INODES_PER_GROUP(sb));

	next3_journal_dirty_metadata(handle, sbi->s_sbh);

exit_journal:
	unlock_super(sb);
	if ((err2 = next3_journal_stop(handle)) && !err)
		err = err2;
	if (!err) {
		update_backups(sb, sbi->s_sbh->b_blocknr, (char *)es,
			       sizeof(struct next3_super_block));
		update_backups(sb, primary->b_blocknr, primary->b_data,
			       primary->b_size);
	}
exit_put:
#ifdef CONFIG_NEXT3_FS_SNAPSHOT_EXCLUDE_INODE
	brelse(exclude_bh);
	iput(exclude_inode);
#endif
	iput(inode);
	return err;
} /* next3_group_add */

/* Extend the filesystem to the new number of blocks specified.  This entry
 * point is only used to extend the current filesystem to the end of the last
 * existing group.  It can be accessed via ioctl, or by "remount,resize=<size>"
 * for emergencies (because it has no dependencies on reserved blocks).
 *
 * If we _really_ wanted, we could use default values to call next3_group_add()
 * allow the "remount" trick to work for arbitrary resizing, assuming enough
 * GDT blocks are reserved to grow to the desired size.
 */
int next3_group_extend(struct super_block *sb, struct next3_super_block *es,
		      next3_fsblk_t n_blocks_count)
{
	next3_fsblk_t o_blocks_count;
	unsigned long o_groups_count;
	next3_grpblk_t last;
	next3_grpblk_t add;
	struct buffer_head * bh;
	handle_t *handle;
	int err;
	unsigned long freed_blocks;

	/* We don't need to worry about locking wrt other resizers just
	 * yet: we're going to revalidate es->s_blocks_count after
	 * taking lock_super() below. */
	o_blocks_count = le32_to_cpu(es->s_blocks_count);
	o_groups_count = NEXT3_SB(sb)->s_groups_count;

	if (test_opt(sb, DEBUG))
		printk(KERN_DEBUG "NEXT3-fs: extending last group from "E3FSBLK" uto "E3FSBLK" blocks\n",
		       o_blocks_count, n_blocks_count);

	if (n_blocks_count == 0 || n_blocks_count == o_blocks_count)
		return 0;

	if (n_blocks_count > (sector_t)(~0ULL) >> (sb->s_blocksize_bits - 9)) {
		printk(KERN_ERR "NEXT3-fs: filesystem on %s:"
			" too large to resize to %lu blocks safely\n",
			sb->s_id, n_blocks_count);
		if (sizeof(sector_t) < 8)
			next3_warning(sb, __func__,
			"CONFIG_LBDAF not enabled\n");
		return -EINVAL;
	}

	if (n_blocks_count < o_blocks_count) {
		next3_warning(sb, __func__,
			     "can't shrink FS - resize aborted");
		return -EBUSY;
	}

	/* Handle the remaining blocks in the last group only. */
	last = (o_blocks_count - le32_to_cpu(es->s_first_data_block)) %
		NEXT3_BLOCKS_PER_GROUP(sb);

	if (last == 0) {
		next3_warning(sb, __func__,
			     "need to use ext2online to resize further");
		return -EPERM;
	}

	add = NEXT3_BLOCKS_PER_GROUP(sb) - last;

	if (o_blocks_count + add < o_blocks_count) {
		next3_warning(sb, __func__, "blocks_count overflow");
		return -EINVAL;
	}

	if (o_blocks_count + add > n_blocks_count)
		add = n_blocks_count - o_blocks_count;

	if (o_blocks_count + add < n_blocks_count)
		next3_warning(sb, __func__,
			     "will only finish group ("E3FSBLK
			     " blocks, %u new)",
			     o_blocks_count + add, add);

	/* See if the device is actually as big as what was requested */
	bh = sb_bread(sb, o_blocks_count + add -1);
	if (!bh) {
		next3_warning(sb, __func__,
			     "can't read last block, resize aborted");
		return -ENOSPC;
	}
	brelse(bh);

	/* We will update the superblock, one block bitmap, and
	 * one group descriptor via next3_free_blocks().
	 */
	handle = next3_journal_start_sb(sb, 3);
	if (IS_ERR(handle)) {
		err = PTR_ERR(handle);
		next3_warning(sb, __func__, "error %d on journal start",err);
		goto exit_put;
	}

	lock_super(sb);
	if (o_blocks_count != le32_to_cpu(es->s_blocks_count)) {
		next3_warning(sb, __func__,
			     "multiple resizers run on filesystem!");
		unlock_super(sb);
		next3_journal_stop(handle);
		err = -EBUSY;
		goto exit_put;
	}

	if ((err = next3_journal_get_write_access(handle,
						 NEXT3_SB(sb)->s_sbh))) {
		next3_warning(sb, __func__,
			     "error %d on journal write access", err);
		unlock_super(sb);
		next3_journal_stop(handle);
		goto exit_put;
	}
	es->s_blocks_count = cpu_to_le32(o_blocks_count + add);
	next3_journal_dirty_metadata(handle, NEXT3_SB(sb)->s_sbh);
	unlock_super(sb);
	next3_debug("freeing blocks %lu through "E3FSBLK"\n", o_blocks_count,
		   o_blocks_count + add);
	next3_free_blocks_sb(handle, sb, o_blocks_count, add, &freed_blocks);
	next3_debug("freed blocks "E3FSBLK" through "E3FSBLK"\n", o_blocks_count,
		   o_blocks_count + add);
	if ((err = next3_journal_stop(handle)))
		goto exit_put;
	if (test_opt(sb, DEBUG))
		printk(KERN_DEBUG "NEXT3-fs: extended group to %u blocks\n",
		       le32_to_cpu(es->s_blocks_count));
	update_backups(sb, NEXT3_SB(sb)->s_sbh->b_blocknr, (char *)es,
		       sizeof(struct next3_super_block));
exit_put:
	return err;
} /* next3_group_extend */
