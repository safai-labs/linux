#
# Makefile for the linux next3-filesystem routines.
#

# default configuration for standalone module
CONFIG_NEXT3_FS?=m
CONFIG_NEXT3_FS_XATTR?=y
CONFIG_NEXT3_FS_POSIX_ACL?=$(CONFIG_FS_POSIX_ACL)
CONFIG_NEXT3_FS_SECURITY?=y
CONFIG_NEXT3_FS_DEBUG?=y

obj-$(CONFIG_NEXT3_FS) += next3.o

next3-y	:= balloc.o bitmap.o dir.o file.o fsync.o ialloc.o inode.o \
	   ioctl.o namei.o super.o symlink.o hash.o resize.o next3_jbd.o
next3-y	+= snapshot.o snapshot_ctl.o buffer.o

next3-$(CONFIG_NEXT3_FS_XATTR)	 += xattr.o xattr_user.o xattr_trusted.o
next3-$(CONFIG_NEXT3_FS_POSIX_ACL) += acl.o
next3-$(CONFIG_NEXT3_FS_SECURITY)	 += xattr_security.o
next3-$(CONFIG_NEXT3_FS_DEBUG)	 += snapshot_debug.o
