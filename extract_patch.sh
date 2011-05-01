#!/bin/sh

RBRANCH=extract_reverse_patches
BRANCH=ext4-snapshot-patches
PATCH=ext4_snapshot_$1.patch
RPATCH=ext4_snapshot_$1-R.patch
CHECKPATCH=./scripts/checkpatch.pl

echo extracting patch $PATCH...

git checkout $RBRANCH~$2 || exit 1

echo $PATCH >> .git/patches/$BRANCH/series


for f in $( ls fs/ext4/* ) ; do
	./strip_ifdefs $f $f.tmp snapshot y || exit 1
done

git checkout $BRANCH || exit 1

rm -f fs/ext4/BUGS.tmp
rm -f fs/ext4/TODO.tmp

#guilt-push
for f in $( ls fs/ext4/*.tmp ) ; do
	file=${f%%.tmp}
	mv -f $f $file|| exit 1
	git add $file || exit 1
done

if [ ! -f .git/patches/$RBRANCH/$RPATCH~ ]; then
	echo 'reverse patch $PATCH~ does not exist'
fi

#Correct mail subject header.
echo "[PATCH " >> .git/patches/$BRANCH/$PATCH~ || exit 1
if [ $3 = y ]; then
echo "RFC " >> .git/patches/$BRANCH/$PATCH~ || exit 1
if
echo "$2/40]" >> .git/patches/$BRANCH/$PATCH~ || exit 1

cat .git/patches/$RBRANCH/$RPATCH~ >> .git/patches/$BRANCH/$PATCH~ 
#Add Signed-off-by lines.
echo '' >> .git/patches/$BRANCH/$PATCH~ ||exit 1
echo 'Signed-off-by: Amir Goldstein <amir73il@users.sf.net>' >> .git/patches/$BRANCH/$PATCH~ || exit 1
echo 'Signed-off-by: Yongqiang Yang <xiaoqiangnk@gmail.com>' >> .git/patches/$BRANCH/$PATCH~ || exit 1

#guilt-refresh
git commit -a -F .git/patches/$BRANCH/$RPATCH~
git show > .git/patches/$BRANCH/$PATCH
$CHECKPATCH .git/patches/$BRANCH/$PATCH >>ext4_snapshot_patches_check
