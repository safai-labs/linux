#!/bin/sh
BASE=ext4-next
RBRANCH=extract_reverse_patches
BRANCH=ext4-snapshot-patches
<<<<<<< HEAD
WORK_BRANCH=for-ext4
RFC=RFC
=======
RFC=y
>>>>>>> 646f21d3b7d3ab7864d91b03f91be6f47fda2226

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

make clean SUBDIRS=fs/ext4
rm -f fs/ext4/*.tmp

NO=1
<<<<<<< HEAD
for key in $( tac KEYS ) ; do
	git checkout $WORK_BRANCH
	./extract_patch.sh $key $NO
=======
for key in $( cat keys ) ; do
	./extract_patch $key $NO $RFC || exit 1
>>>>>>> 646f21d3b7d3ab7864d91b03f91be6f47fda2226
	NO=`expr $NO + 1`
done

NO=`expr $NO - 1`
git checkout $BRANCH
git format-patch --subject-prefix="PATCH $RFC" -n -o .git/patches/$BRANCH/ -$NO || exit 1
