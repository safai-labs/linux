#!/bin/sh
# strip fake ifdefs from ext4-snapshots branch

ORIGIN=ext4-stable
BASE=ext4-next
PATCH=ext4_snapshots.patch

# re-create the strip_ifdefs branch from current branch
(git branch | grep strip_ifdefs) && (git branch -D strip_ifdefs || exit 1)
git checkout -b strip_ifdefs || exit 1

make clean SUBDIRS=fs/ext4
rm -f fs/ext4/*.tmp
gcc -o strip_ifdefs strip_ifdefs.c

echo "stripping fake snapshot ifdefs from ext4 files..."
# strip all SNAPSHOT ifdefs from ext4 files
for f in $( ls fs/ext4/* ) ; do
	./strip_ifdefs $f $f.tmp snapshot y || exit 1
	mv -f $f.tmp $f || exit 1
done

git commit -a -m "stripped fake SNAPSHOT ifdefs"

# create one big snapshots patch and run it through checkpatch
echo "creating $PATCH..."
git diff -b $ORIGIN fs/ext4 > $PATCH
echo "ext4 files changed by snapshots patch:"
git diff --stat --diff-filter=M -b $BASE fs/ext4

echo "checking $PATCH..."
./scripts/checkpatch.pl $PATCH | tee $PATCH.check | tail

