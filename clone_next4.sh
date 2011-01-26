#!/bin/sh

rm -rf fs/next4
mkdir -p fs/next4
cp -a fs/ext4/*.h fs/next4
cp -a fs/ext4/*.c fs/next4
cp -a fs/ext4/Kconfig fs/next4
cp -a fs/ext4/Makefile fs/next4
cp -a include/trace/events/ext4.h include/trace/events/next4.h
cd fs/next4
rm *.mod.c
mv ext4_extents.h next4_extents.h
mv ext4_jbd2.h next4_jbd2.h
mv ext4_jbd2.c next4_jbd2.c
mv ext4.h next4.h
cd -
sed -f next4.sed -i fs/next4/*
sed -f next4.sed -i include/trace/events/next4.h
