#!/bin/sh

KERNEL=$(uname -r)
KDIR=/lib/modules/${KERNEL}/build

sudo cp fs/ext4dev/ext4dev_events.h ${KDIR}/include/trace/events/ext4dev.h
make -C ${KDIR} M=${PWD}/fs/ext4dev modules
sudo make -C ${KDIR} M=${PWD}/fs/ext4dev modules_install
sudo rmmod ext4dev
sudo modprobe ext4dev

