#!/bin/sh
BASE=ext4-next
RBRANCH=extract_reverse_patches
BRANCH=extract_patches
RFC=RFC

if [ ! -d .git/patches/$RBRANCH ]; then
	echo 'reverse patches must exist before doing this.'
	exit 1
fi

# create work copies of scripts/KEYS before switching branches
gcc -o strip_ifdefs strip_ifdefs.c || exit 1
tac KEYS > keys || exit 1
cp extract_patch.sh extract_patch || exit 1

# re-create the branch from current head
(git branch | grep $BRANCH) && (git branch -D $BRANCH || exit 1)
git checkout -b $BRANCH $BASE || exit 1

echo -n >ext4_snapshot_patches_check
#guilt-init
#guilt-pop -a
mkdir -p .git/patches/$BRANCH
echo -n > .git/patches/$BRANCH/series
echo -n > .git/patches/$BRNACH/status

# create forward and reverse work dirs
rm -f fs/ext4*
git checkout $RBRANCH fs/ext4 || exit 1
mv fs/ext4 fs/ext4.rev
git checkout $BRANCH fs/ext4 || exit 1

NO=1
for key in $( cat keys ) ; do
	./extract_patch $key $NO $1 || exit 1
	NO=`expr $NO + 1`
done

#NO=`expr $NO - 1`
#git checkout $BRANCH
git format-patch --subject-prefix="PATCH $RFC" -n -o .git/patches/$BRANCH/ $BASE..$BRANCH || exit 1
