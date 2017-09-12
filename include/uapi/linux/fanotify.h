#ifndef _UAPI_LINUX_FANOTIFY_H
#define _UAPI_LINUX_FANOTIFY_H

#include <linux/types.h>

/* the following events that user-space can register for */
#define FAN_ACCESS		0x00000001	/* File was accessed */
#define FAN_MODIFY		0x00000002	/* File was modified */
#define FAN_ATTRIB		0x00000004	/* Metadata changed */
#define FAN_CLOSE_WRITE		0x00000008	/* Writtable file closed */
#define FAN_CLOSE_NOWRITE	0x00000010	/* Unwrittable file closed */
#define FAN_OPEN		0x00000020	/* File was opened */
#define FAN_MOVED_FROM		0x00000040	/* File was moved from X */
#define FAN_MOVED_TO		0x00000080	/* File was moved to Y */
#define FAN_CREATE		0x00000100	/* Subfile was created */
#define FAN_DELETE		0x00000200	/* Subfile was deleted */
#define FAN_DELETE_SELF		0x00000400	/* Self was deleted */
#define FAN_MOVE_SELF		0x00000800	/* Self was moved */


#define FAN_Q_OVERFLOW		0x00004000	/* Event queued overflowed */

#define FAN_OPEN_PERM		0x00010000	/* File open in perm check */
#define FAN_ACCESS_PERM		0x00020000	/* File accessed in perm check */

#define FAN_ONDIR		0x40000000	/* event occurred against dir */

#define FAN_EVENT_ON_CHILD	0x08000000	/* interested in child events */

/* helper events */
#define FAN_CLOSE		(FAN_CLOSE_WRITE | FAN_CLOSE_NOWRITE) /* close */
#define FAN_MOVE		(FAN_MOVED_FROM | FAN_MOVED_TO) /* moves */

/* flags used for fanotify_init() */
#define FAN_CLOEXEC		0x00000001
#define FAN_NONBLOCK		0x00000002

/* These are NOT bitwise flags.  Both bits are used togther.  */
#define FAN_CLASS_NOTIF		0x00000000
#define FAN_CLASS_CONTENT	0x00000004
#define FAN_CLASS_PRE_CONTENT	0x00000008
#define FAN_ALL_CLASS_BITS	(FAN_CLASS_NOTIF | FAN_CLASS_CONTENT | \
				 FAN_CLASS_PRE_CONTENT)

#define FAN_UNLIMITED_QUEUE	0x00000010
#define FAN_UNLIMITED_MARKS	0x00000020

/* These bits determine the format of the reported events */
#define FAN_EVENT_INFO_PARENT	0x00000100	/* Event fd maybe of parent */
#define FAN_EVENT_INFO_NAME	0x00000200	/* Event data has filename */
#define FAN_ALL_EVENT_INFO_BITS (FAN_EVENT_INFO_PARENT | \
				 FAN_EVENT_INFO_NAME)

#define FAN_ALL_INIT_FLAGS	(FAN_CLOEXEC | FAN_NONBLOCK | \
				 FAN_ALL_CLASS_BITS | \
				 FAN_ALL_EVENT_INFO_BITS | \
				 FAN_UNLIMITED_QUEUE | FAN_UNLIMITED_MARKS)

/* flags used for fanotify_modify_mark() */
#define FAN_MARK_ADD		0x00000001
#define FAN_MARK_REMOVE		0x00000002
#define FAN_MARK_DONT_FOLLOW	0x00000004
#define FAN_MARK_ONLYDIR	0x00000008
#define FAN_MARK_MOUNT		0x00000010
#define FAN_MARK_IGNORED_MASK	0x00000020
#define FAN_MARK_IGNORED_SURV_MODIFY	0x00000040
#define FAN_MARK_FLUSH		0x00000080

#define FAN_ALL_MARK_FLAGS	(FAN_MARK_ADD |\
				 FAN_MARK_REMOVE |\
				 FAN_MARK_DONT_FOLLOW |\
				 FAN_MARK_ONLYDIR |\
				 FAN_MARK_MOUNT |\
				 FAN_MARK_IGNORED_MASK |\
				 FAN_MARK_IGNORED_SURV_MODIFY |\
				 FAN_MARK_FLUSH)

/*
 * All of the events - we build the list by hand so that we can add flags in
 * the future and not break backward compatibility.  Apps will get only the
 * events that they originally wanted.  Be sure to add new events here!
 */

/* Events reported with data type FSNOTIFY_EVENT_PATH */
#define FAN_PATH_EVENTS (FAN_ACCESS |\
			 FAN_MODIFY |\
			 FAN_CLOSE |\
			 FAN_OPEN)

/* Events reported with data type FSNOTIFY_EVENT_DENTRY */
#define FAN_DENTRY_EVENTS (FAN_ATTRIB |\
			   FAN_MOVE | FAN_MOVE_SELF |\
			   FAN_CREATE | FAN_DELETE)

#define FAN_ALL_EVENTS (FAN_PATH_EVENTS | FAN_DENTRY_EVENTS)

/*
 * All events which require a permission response from userspace
 */
#define FAN_ALL_PERM_EVENTS (FAN_OPEN_PERM |\
			     FAN_ACCESS_PERM)

/* Events on directory requiring to pass filename */
#define FAN_FILENAME_EVENTS (FAN_MOVED_FROM | FAN_MOVED_TO |\
			     FAN_CREATE | FAN_DELETE)

#define FAN_ALL_OUTGOING_EVENTS	(FAN_ALL_EVENTS |\
				 FAN_ALL_PERM_EVENTS |\
				 FAN_Q_OVERFLOW)

#define FANOTIFY_METADATA_VERSION	3

struct fanotify_event_metadata {
	__u32 event_len;
	__u8 vers;
	__u8 reserved;
	__u16 metadata_len;
	__aligned_u64 mask;
	__s32 fd;
	__s32 pid;
};

struct fanotify_response {
	__s32 fd;
	__u32 response;
};

/* Legit userspace responses to a _PERM event */
#define FAN_ALLOW	0x01
#define FAN_DENY	0x02
/* No fd set in event */
#define FAN_NOFD	-1

/* Helper functions to deal with fanotify_event_metadata buffers */
#define FAN_EVENT_METADATA_LEN (sizeof(struct fanotify_event_metadata))

#define FAN_EVENT_NEXT(meta, len) ((len) -= (meta)->event_len, \
				   (struct fanotify_event_metadata*)(((char *)(meta)) + \
				   (meta)->event_len))

#define FAN_EVENT_OK(meta, len)	((long)(len) >= (long)FAN_EVENT_METADATA_LEN && \
				(long)(meta)->event_len >= (long)FAN_EVENT_METADATA_LEN && \
				(long)(meta)->event_len <= (long)(len))

#endif /* _UAPI_LINUX_FANOTIFY_H */
