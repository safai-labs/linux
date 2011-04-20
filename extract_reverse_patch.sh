#!/bin/sh

BRANCH=extract_reverse_patches
PATCH=ext4_snapshot_$1-R.patch

echo extracting reverse patch $PATCH...

echo $PATCH >> .git/patches/$BRANCH/series
./strip_ifdefs fs/ext4/Kconfig /dev/null $1 > .git/patches/$BRANCH/$PATCH~ || exit 1

#guilt-push

for f in $( ls fs/ext4/* ) ; do
	./strip_ifdefs $f $f.tmp $1 || exit 1
	mv -f $f.tmp $f || exit 1
done

#guilt-refresh
git commit -a -F .git/patches/$BRANCH/$PATCH~
git show > .git/patches/$BRANCH/$PATCH
