#!/bin/sh

KERNEL=$(uname -r)
KDIR=/lib/modules/${KERNEL}/build

sudo cp fs/next4/next4_events.h ${KDIR}/include/trace/events/next4.h
make -C ${KDIR} M=${PWD}/fs/next4 modules
sudo make -C ${KDIR} M=${PWD}/fs/next4 modules_install
sudo rmmod next4
sudo modprobe next4

