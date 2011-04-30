BASE=ext4-next
PATCH=ext4_snapshots.patch
BRANCH=ext4-snapshot-patches

# re-create the ext4-snapshot-patches branch from ext4-next branch
(git branch | grep $BRANCH) && (git branch -D $BRANCH || exit 1)
git checkout -b $BRANCH $BASE|| exit 1

