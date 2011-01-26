#!/bin/sh

make modules_prepare
make modules SUBDIRS=fs/ext4
