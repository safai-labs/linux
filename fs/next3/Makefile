#
# Makefile for the linux next3-filesystem routines.
#

obj-$(CONFIG_NEXT3_FS) += next3.o

next3-y	:= balloc.o bitmap.o dir.o file.o fsync.o ialloc.o inode.o \
	   ioctl.o namei.o super.o symlink.o hash.o resize.o next3_jbd.o

next3-$(CONFIG_NEXT3_FS_XATTR)	 += xattr.o xattr_user.o xattr_trusted.o
next3-$(CONFIG_NEXT3_FS_POSIX_ACL) += acl.o
next3-$(CONFIG_NEXT3_FS_SECURITY)	 += xattr_security.o
next3-$(CONFIG_NEXT3_FS_SNAPSHOT)	 += snapshot.o
next3-$(CONFIG_NEXT3_FS_SNAPSHOT)	 += snapshot_ctl.o
next3-$(CONFIG_NEXT3_FS_SNAPSHOT_DEBUG)	 += snapshot_debug.o
