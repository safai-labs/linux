#!/bin/sh

PATCH=ext4_snapshot_$1-R.patch

echo extracting reverse patch $PATCH...

echo $PATCH >> .git/patches/ext4-snapshots/series
./strip_ifdefs fs/ext4/Kconfig /dev/null $1 > .git/patches/ext4-snapshots/$PATCH

guilt-push

for f in $( ls fs/ext4/* ) ; do
	./strip_ifdefs $f $f.tmp $1 || exit 1
	mv -f $f.tmp $f
done

guilt-refresh
