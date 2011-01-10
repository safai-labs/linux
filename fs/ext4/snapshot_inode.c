#include "ext4_jbd2.h"
#include "ext4_extents.h"

#include <trace/events/ext4.h>
#include "snapshot.h"

#ifdef CONFIG_EXT4_FS_SNAPSHOT_CLEANUP_SHRINK
/**
 * ext4_blks_to_skip - count the number blocks that can be skipped
 * @inode: inode in question
 * @i_block: start block number
 * @maxblocks: max number of data blocks to be skipped
 * @chain: chain of indirect blocks
 * @depth: length of chain from inode to data block
 * @offsets: array of offsets in chain blocks
 * @k: number of allocated blocks in the chain
 *
 * Counts the number of non-allocated data blocks (holes) at offset @i_block.
 * Called from ext4_snapshot_merge_blocks() and ext4_snapshot_shrink_blocks()
 * under snapshot_mutex.
 * Returns the total number of data blocks to be skipped.
 */

static int ext4_blks_to_skip(struct inode *inode, long i_block,
		unsigned long maxblocks, Indirect chain[4], int depth,
		int *offsets, int k)
{
	int ptrs = EXT4_ADDR_PER_BLOCK(inode->i_sb);
	int ptrs_bits = EXT4_ADDR_PER_BLOCK_BITS(inode->i_sb);
	const long direct_blocks = EXT4_NDIR_BLOCKS,
		indirect_blocks = ptrs,
		double_blocks = (1 << (ptrs_bits * 2));
	/* number of data blocks mapped with a single splice to the chain */
	int data_ptrs_bits = ptrs_bits * (depth - k - 1);
	int max_ptrs = maxblocks >> data_ptrs_bits;
	int final = 0;
	unsigned long count = 0;

	switch (depth) {
	case 4: /* tripple indirect */
		i_block -= double_blocks;
		/* fall through */
	case 3: /* double indirect */
		i_block -= indirect_blocks;
		/* fall through */
	case 2: /* indirect */
		i_block -= direct_blocks;
		final = (k == 0 ? 1 : ptrs);
		break;
	case 1: /* direct */
		final = direct_blocks;
		break;
	}
	/* offset of block from start of splice point */
	i_block &= ((1 << data_ptrs_bits) - 1);
	/* up to 4 triple indirect blocks are used to map 2^32 blocks */
	if (ext4_snapshot_file(inode) && depth == 4 && k == 0)
		final = EXT4_SNAPSHOT_NTIND_BLOCKS;

	count++;
	while (count <= max_ptrs &&
		offsets[k] + count < final &&
		le32_to_cpu(*(chain[k].p + count)) == 0) {
		count++;
	}
	/* number of data blocks mapped by 'count' splice points */
	count <<= data_ptrs_bits;
	count -= i_block;
	return count < maxblocks ? count : maxblocks;
}

/*
 * ext4_snapshot_shrink_blocks - free unused blocks from deleted snapshot
 * @handle: JBD handle for this transaction
 * @inode:	inode we're shrinking
 * @iblock:	inode offset to first data block to shrink
 * @maxblocks:	inode range of data blocks to shrink
 * @cow_bh:	buffer head to map the COW bitmap block
 *		if NULL, don't look for COW bitmap block
 * @shrink:	shrink mode: 0 (don't free), >0 (free unused), <0 (free all)
 * @pmapped:	return no. of mapped blocks or 0 for skipped holes
 *
 * Frees @maxblocks blocks starting at offset @iblock in @inode, which are not
 * 'in-use' by non-deleted snapshots (blocks 'in-use' are set in COW bitmap).
 * If @shrink is false, just count mapped blocks and look for COW bitmap block.
 * The first time that a COW bitmap block is found in @inode, whether @inode is
 * deleted or not, it is stored in @cow_bh and is used in subsequent calls to
 * this function with other deleted snapshots within the block group boundaries.
 * Called from ext4_snapshot_shrink_blocks() under snapshot_mutex.
 *
 * Return values:
 * >= 0 - no. of shrunk blocks (*@pmapped ? mapped blocks : skipped holes)
 *  < 0 - error
 */
int ext4_snapshot_shrink_blocks(handle_t *handle, struct inode *inode,
		sector_t iblock, unsigned long maxblocks,
		struct buffer_head *cow_bh,
		int shrink, int *pmapped)
{
	int offsets[4];
	Indirect chain[4], *partial;
	int err, blocks_to_boundary, depth, count;
	struct buffer_head *sbh = NULL;
	struct ext4_group_desc *desc = NULL;
	ext4_snapblk_t block_bitmap, block = SNAPSHOT_BLOCK(iblock);
	unsigned long block_group = SNAPSHOT_BLOCK_GROUP(block);
	int mapped_blocks = 0, freed_blocks = 0;
	const char *cow_bitmap;

	BUG_ON(shrink &&
		(!(EXT4_I(inode)->i_flags & EXT4_SNAPFILE_DELETED_FL) ||
		ext4_snapshot_is_active(inode)));

	depth = ext4_block_to_path(inode, iblock, offsets,
			&blocks_to_boundary);
	if (depth == 0)
		return -EIO;

	desc = ext4_get_group_desc(inode->i_sb, block_group, NULL);
	if (!desc)
		return -EIO;
	block_bitmap = ext4_block_bitmap(inode->i_sb, desc);
	partial = ext4_get_branch(inode, depth, offsets, chain, &err);
	if (err)
		return err;

	if (partial) {
		/* block not mapped (hole) - count the number of holes to
		 * skip */
		count = ext4_blks_to_skip(inode, iblock, maxblocks, chain,
					   depth, offsets, (partial - chain));
		snapshot_debug(3, "skipping snapshot (%u) blocks: block=0x%llx"
			       ", count=0x%x\n", inode->i_generation,
			       block, count);
		goto shrink_indirect_blocks;
	}

	/* data block mapped - check if data blocks should be freed */
	partial = chain + depth - 1;
	/* scan all blocks upto maxblocks/boundary */
	count = 0;
	while (count < maxblocks && count <= blocks_to_boundary) {
		ext4_fsblk_t blk = le32_to_cpu(*(partial->p + count));
		if (blk && block + count == block_bitmap &&
			cow_bh && !buffer_mapped(cow_bh)) {
			/*
			 * 'blk' is the COW bitmap physical block -
			 * store it in cow_bh for subsequent calls
			 */
			map_bh(cow_bh, inode->i_sb, blk);
			set_buffer_new(cow_bh);
			snapshot_debug(3, "COW bitmap #%lu: snapshot "
				"(%u), bitmap_blk=(+%lld)\n",
				block_group, inode->i_generation,
				SNAPSHOT_BLOCK_GROUP_OFFSET(block_bitmap));
		}
		if (blk)
			/* count mapped blocks in range */
			mapped_blocks++;
		else if (shrink >= 0)
			/*
			 * Unless we are freeing all block in range,
			 * we cannot have holes inside mapped range
			 */
			break;
		/* count size of range */
		count++;
	}

	if (!shrink)
		goto done_shrinking;

	cow_bitmap = NULL;
	if (shrink > 0 && cow_bh && buffer_mapped(cow_bh)) {
		/* we found COW bitmap - consult it when shrinking */
		sbh = sb_bread(inode->i_sb, cow_bh->b_blocknr);
		if (!sbh) {
			err = -EIO;
			goto cleanup;
		}
		cow_bitmap = sbh->b_data;
	}
	if (shrink < 0 || cow_bitmap) {
		int bit = SNAPSHOT_BLOCK_GROUP_OFFSET(block);

		BUG_ON(bit + count > SNAPSHOT_BLOCKS_PER_GROUP);
		/* free blocks with or without consulting COW bitmap */
		ext4_free_data_cow(handle, inode, partial->bh,
				partial->p, partial->p + count,
				cow_bitmap, bit, &freed_blocks, NULL);
	}

shrink_indirect_blocks:
	/* check if the indirect block should be freed */
	if (shrink && partial == chain + depth - 1) {
		Indirect *ind = partial - 1;
		__le32 *p = NULL;
		if (freed_blocks == mapped_blocks &&
		    count > blocks_to_boundary) {
			for (p = (__le32 *)(partial->bh->b_data);
			     !*p && p < partial->p; p++)
				;
		}
		if (p == partial->p)
			/* indirect block maps zero data blocks - free it */
			ext4_free_data(handle, inode, ind->bh, ind->p,
					ind->p+1);
	}

done_shrinking:
	snapshot_debug(3, "shrinking snapshot (%u) blocks: shrink=%d, "
			"block=0x%llx, count=0x%x, mapped=0x%x, freed=0x%x\n",
			inode->i_generation, shrink, block, count,
			mapped_blocks, freed_blocks);

	if (pmapped)
		*pmapped = mapped_blocks;
	err = count;
cleanup:
	while (partial > chain) {
		brelse(partial->bh);
		partial--;
	}
	brelse(sbh);
return err;
}

#endif
