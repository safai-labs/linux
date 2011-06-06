/*
 * linux/fs/ext4/snapshot_ctl.c
 *
 * Written by Amir Goldstein <amir73il@users.sf.net>, 2008
 *
 * Copyright (C) 2008-2011 CTERA Networks
 *
 * This file is part of the Linux kernel and is made available under
 * the terms of the GNU General Public License, version 2, or at your
 * option, any later version, incorporated herein by reference.
 *
 * Ext4 snapshots control functions.
 */

#include <linux/statfs.h>
#include "ext4_jbd2.h"
#include "snapshot.h"
#define ext4_snapshot_reset_bitmap_cache(sb, init) 0

/*
 * Snapshot constructor/destructor
 */
