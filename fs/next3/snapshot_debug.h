//#pragma ezk
/*
 * linux/fs/next3/snapshot_debug.h
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

#ifndef _LINUX_NEXT3_SNAPSHOT_DEBUG_H
#define _LINUX_NEXT3_SNAPSHOT_DEBUG_H

#ifdef CONFIG_NEXT3_FS_SNAPSHOT_DEBUG
#include <linux/delay.h>

#warning typo? "IDENT" below should be "INDENT" with an "N". Right? if so, fix it everywhere you say "ident" instead of "indent" (e.g., snapshot_ident extern)
#define SNAPSHOT_IDENT_MAX 4
#define SNAPSHOT_IDENT_STR "\t\t\t\t"
#define KERN_LEVEL_STR "<%d>"
#define SNAP_KERN_LEVEL(n) ((n)+2) /* 1 = KERN_ERR, ..., 5 = KERN_DEBUG */

#define SNAPTEST_TAKE	0
#define SNAPTEST_DELETE	1
#define SNAPTEST_COW	2
#define SNAPTEST_READ	3
#define SNAPTEST_BITMAP	4
#define SNAPSHOT_TESTS_NUM	5

extern const char *snapshot_ident;
extern u8 snapshot_enable_debug;
extern u16 snapshot_enable_test[SNAPSHOT_TESTS_NUM];

#define snapshot_test_delay(i)		     \
	do {							       \
		if (snapshot_enable_test[i])			       \
			msleep_interruptible(snapshot_enable_test[i]); \
	} while (0)

#define snapshot_test_delay_per_ticks(i, n)	    \
	do {								\
		if (snapshot_enable_test[i] && (n))			\
			msleep_interruptible(				\
				(snapshot_enable_test[i]/(n))+1);	\
	} while (0)

#define snapshot_debug_l(n, l, f, a...)					\
	do {								\
		if ((n) <= snapshot_enable_debug &&			\
		    (l) <= SNAPSHOT_IDENT_MAX) {			\
			printk(KERN_LEVEL_STR "snapshot: %s" f,		\
				   SNAP_KERN_LEVEL(n),			\
			       snapshot_ident - (l),			\
				   ## a);				\
		}							\
	} while (0)

#define snapshot_debug(n, f, a...)	snapshot_debug_l(n, 0, f, ## a)

#define SNAPSHOT_DEBUG_ONCE int once = 1
#define snapshot_debug_once(n, f, a...)					\
	do {								\
		if (once) {						\
			snapshot_debug(n, f, ## a);			\
			once = 0;					\
		}							\
	} while (0)

#else
#define snapshot_test_delay(i)
#define snapshot_test_delay_per_ticks(i, n)
#define snapshot_debug(n, f, a...)
#define snapshot_debug_l(n, l, f, a...)
#define snapshot_debug_once(n, f, a...)
#define SNAPSHOT_DEBUG_ONCE
#endif

/* debug levels */
#define SNAP_ERR	1 /* errors and summary */
#define SNAP_WARN	2 /* warnings */
#define SNAP_INFO	3 /* info */
#define SNAP_DEBUG	4 /* debug */
#define SNAP_DUMP	5 /* dump snapshot file */

#endif	/* _LINUX_NEXT3_SNAPSHOT_DEBUG_H */
