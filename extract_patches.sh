#!/bin/sh
BASE=ext4-next
RBRANCH=extract_reverse_patches
BRANCH=ext4-snapshot-patches
WORK_BRANCH=for-ext4
RFC=y

# re-create the branch from current head
(git branch | grep $BRANCH) && (git branch -D $BRANCH || exit 1)
git branch $BRANCH $BASE|| exit 1

if [ ! -d .git/patches/$RBRANCH ]; then
	echo 'reverse patches must be exist before doing this.'
	exit 1
fi

echo -n >ext4_snapshot_patches_check
#guilt-init
#guilt-pop -a
mkdir -p .git/patches/$BRANCH
echo -n > .git/patches/$BRANCH/series
echo -n > .git/patches/$BRNACH/status

make clean SUBDIRS=fs/ext4
rm -f fs/ext4/*.tmp

NO=1
for key in $( tac KEYS ) ; do
	git checkout $WORK_BRANCH
	./extract_patch.sh $key $NO $RFC
	NO=`expr $NO + 1`
done
