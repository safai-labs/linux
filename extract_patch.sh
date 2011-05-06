#!/bin/sh

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
	echo 'reverse patch $RPATCH~ does not exist'
fi

cat .git/patches/$RBRANCH/$RPATCH~ > .git/patches/$QBRANCH/$PATCH~ 
#Add Signed-off-by lines.
echo '' >> .git/patches/$QBRANCH/$PATCH~ || exit 1
echo 'Signed-off-by: Amir Goldstein <amir73il@users.sf.net>' >> .git/patches/$QBRANCH/$PATCH~ || exit 1
echo 'Signed-off-by: Yongqiang Yang <xiaoqiangnk@gmail.com>' >> .git/patches/$QBRANCH/$PATCH~ || exit 1

cat .git/patches/$QBRANCH/$PATCH~ > .git/patches/$QBRANCH/$PATCH
echo '' >> .git/patches/$QBRANCH/$PATCH || exit 1
git diff --cached >> .git/patches/$QBRANCH/$PATCH || exit 1
cat .git/patches/$QBRANCH/$PATCH~
git diff --cached --stat

#guilt-refresh
git commit -a --author='Amir Goldstein <amir73il@users.sf.net>' -F .git/patches/$QBRANCH/$PATCH~ || exit 0
echo $PATCH >> .git/patches/$QBRANCH/series

echo checking patch $PATCH...
$CHECKPATCH .git/patches/$QBRANCH/$PATCH >> ext4_snapshot_patches_check

echo
echo patch $PATCH applied.
echo
