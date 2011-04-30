#!/bin/sh

RBRANCH=extract_reverse_patches
BRANCH=ext4-snapshot-patches
PATCH=ext4_snapshot_$1.patch
RPATCH=ext4_snapshot_$1-R.patch
CHECKPATCH=./scripts/checkpatch.pl

echo extracting patch $PATCH...

git checkout $RBRANCH~$2

echo $PATCH >> .git/patches/$BRANCH/series


for f in $( ls fs/ext4/* ) ; do
	cp -f $f $f.tmp|| exit 1
done

git checkout $BRANCH

#guilt-push
for f in $( ls fs/ext4/*[^tmp] ) ; do
	mv -f $f.tmp $f|| exit 1
	rm -f $f.tmp
done

rm -f fs/ext4/*.tmp

if [ ! -f .git/patches/$RBRANCH/$RPATCH~ ]; then
	echo 'reverse patch $PATCH~ does not exist'
fi

#guilt-refresh
git commit -a -s -F .git/patches/$RBRANCH/$RPATCH~
git show > .git/patches/$BRANCH/$PATCH
$CHECKPATCH .git/patches/$BRANCH/$PATCH >>ext4_snapshot_patches_check
