#!/bin/sh

BRANCH=extract_reverse_patches

# re-create the branch from current head
(git branch | grep $BRANCH) && (git branch -d $BRANCH || exit 1)
git checkout -b $BRANCH || exit 1

gcc -o strip_ifdefs strip_ifdefs.c

#guilt-init
#guilt-pop -a
mkdir -p .git/patches/$BRANCH
echo -n > .git/patches/$BRANCH/series
echo -n > .git/patches/$BRNACH/status

make clean SUBDIRS=fs/ext4
rm -f fs/ext4/*.tmp

for key in $( cat KEYS ) ; do
	./extract_reverse_patch.sh $key || exit 1
done
