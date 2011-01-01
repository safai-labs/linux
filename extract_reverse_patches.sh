#!/bin/sh

gcc -o strip_ifdefs strip_ifdefs.c

guilt-init
guilt-pop -a
echo -n > .git/patches/ext4-snapshots/series
echo -n > .git/patches/ext4-snapshots/status

for key in $( cat KEYS ) ; do
	./extract_reverse_patch.sh $key
done
