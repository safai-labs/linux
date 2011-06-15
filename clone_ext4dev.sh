#!/bin/sh

# re-create the clone_ext4dev branch from current branch
(git branch | grep clone_ext4dev) && (git branch -D clone_ext4dev || exit 1)
git checkout -b clone_ext4dev

rm -rf fs/ext4dev
mkdir -p fs/ext4dev
cp -a fs/ext4/*.h fs/ext4dev
cp -a fs/ext4/*.c fs/ext4dev
cp -a fs/ext4/Kconfig fs/ext4dev
cp -a fs/ext4/Makefile fs/ext4dev
cp -a include/trace/events/ext4.h fs/ext4dev/ext4dev_events.h
cd fs/ext4dev
rm *.mod.c 2>/dev/null
mv ext4_extents.h ext4dev_extents.h
mv ext4_jbd2.h ext4dev_jbd2.h
mv ext4_jbd2.c ext4dev_jbd2.c
mv ext4.h ext4dev.h
sed -f ../../ext4dev.sed -i *
cd ..
tar cfz ../ext4dev_module.tar.gz ext4dev/
cd ..
cp -a fs/ext4dev/ext4dev_events.h include/trace/events/ext4dev.h

git add fs/ext4dev include/trace/events/ext4dev.h
git commit -m "add ext4dev fs"
git diff v2.6.38 fs/Makefile fs/ext4dev include/trace/events/ext4dev.h > ext4dev_fs.patch
