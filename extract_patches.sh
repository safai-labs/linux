#!/bin/sh
BASE=ext4-next
REBASE=ext4-dev
VER=v1
export RBRANCH=extract_reverse_patches
export BRANCH=extract_patches
export QBRANCH=for-ext4-$VER

if [ ! -d .git/patches/$RBRANCH ]; then
	echo 'reverse patches must exist before doing this.'
	exit 1
fi

# create work copies of scripts/KEYS before switching branches
gcc -o strip_ifdefs strip_ifdefs.c || exit 1
tac KEYS > keys || exit 1
cp extract_patch.sh extract_patch || exit 1

# re-create the branch from base head
(git branch | grep $BRANCH) && (git branch -D $BRANCH || exit 1)
git checkout -b $BRANCH $BASE || exit 1

echo -n > ext4_snapshot_patches_check
#guilt-init
#guilt-pop -a
# prepare patches queue to apply on top of ext4/master
mkdir -p .git/patches/$QBRANCH
echo -n > .git/patches/$QBRANCH/series
echo -n > .git/patches/$QBRANCH/status

# create forward and reverse work dirs
rm -rf fs/ext4*
git checkout $RBRANCH fs/ext4 || exit 1
mv fs/ext4 fs/ext4.rev
git checkout $BRANCH fs/ext4 || exit 1

NO=1
for key in $( cat keys ) ; do
	./extract_patch $key $NO $1 || exit 1
	NO=`expr $NO + 1`
done

# exclude journal_error and ctl_dump debug patches from snapshot patch series
git format-patch --subject-prefix="PATCH $VER" -n -o .git/patches/$QBRANCH/ $BASE..$BRANCH~2 || exit 1

# re-create the branch from rebase head
(git branch | grep $QBRANCH) && (git branch -D $QBRANCH || exit 1)
git checkout -b $QBRANCH $REBASE || exit 1

# try to apply all patch queue
guilt-push -a
# exclude journal_error and ctl_dump debug patches from snapshot patch series
guilt-pop
guilt-pop

git format-patch --subject-prefix="PATCH $VER" -n $REBASE..$QBRANCH || exit 1
