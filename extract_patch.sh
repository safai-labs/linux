#!/bin/sh

RBRANCH=extract_reverse_patches
BRANCH=extract_patches
PATCH=ext4_snapshot_$1.patch
RPATCH=ext4_snapshot_$1-R.patch
CHECKPATCH=./scripts/checkpatch.pl

echo
echo extracting patch $PATCH...
echo 

#git checkout $RBRANCH~$2 fs/ext4 || exit 1

# apply reverse patch on reverse work dir
patch -R -p3 -d fs/ext4.rev < .git/patches/$RBRANCH/$RPATCH || exit 1

# strip fake ifdefs and copy to forward work dir
for f in $( ls fs/ext4.rev ) ; do
	./strip_ifdefs fs/ext4.rev/$f fs/ext4/$f snapshot y 2>/dev/null || exit 1
done

#git checkout $BRANCH || exit 1

# 'core' patch series doesn't include added snapshot C files
if [ _$3 = _core ] ; then
	rm -f fs/ext4/snapshot*.c
fi

#guilt-push
for f in $( ls fs/ext4/* ) ; do
#	file=${f%%.tmp}
#	mv -f $f $file|| exit 1
	git add $f || exit 1
done

if [ ! -f .git/patches/$RBRANCH/$RPATCH~ ]; then
	echo 'reverse patch $PATCH~ does not exist'
fi

cat .git/patches/$RBRANCH/$RPATCH~ > .git/patches/$BRANCH/$PATCH~ 
#Add Signed-off-by lines.
echo '' >> .git/patches/$BRANCH/$PATCH~ || exit 1
echo 'Signed-off-by: Amir Goldstein <amir73il@users.sf.net>' >> .git/patches/$BRANCH/$PATCH~ || exit 1
echo 'Signed-off-by: Yongqiang Yang <xiaoqiangnk@gmail.com>' >> .git/patches/$BRANCH/$PATCH~ || exit 1

cat .git/patches/$BRANCH/$PATCH~ > .git/patches/$BRANCH/$PATCH
echo '' >> .git/patches/$BRANCH/$PATCH || exit 1
git diff --cached >> .git/patches/$BRANCH/$PATCH || exit 1
cat .git/patches/$BRANCH/$PATCH~
git diff --cached --stat

#guilt-refresh
git commit -a --author='Amir Goldstein <amir73il@users.sf.net>' -F .git/patches/$BRANCH/$PATCH~ || exit 0
echo $PATCH >> .git/patches/$BRANCH/series

echo checking patch $PATCH...
$CHECKPATCH .git/patches/$BRANCH/$PATCH >> ext4_snapshot_patches_check

echo
echo patch $PATCH applied.
echo
