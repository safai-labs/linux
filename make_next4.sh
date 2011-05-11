#!/bin/sh

KERNEL=$(uname -r)
KDIR=/lib/modules/${KERNEL}/build

sudo cp include/trace/events/next4.h ${KDIR}/include/trace/events/
make -C ${KDIR} M=${PWD}/fs/next4 modules
sudo make -C ${KDIR} M=${PWD}/fs/next4 modules_install
sudo rmmod next4
sudo modprobe next4

