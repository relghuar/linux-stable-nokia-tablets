#!/bin/bash

. `dirname $0`/init.sh

RDIR=${BDIR}/rootfs

echo "make..."
time $BDIR/make || exit 1

echo "modules..."
rm -rf $RDIR/usr/lib/modules
time $BDIR/make modules_install || exit 1

echo "image..."
rm -f $RDIR/boot/$KNAME
mkdir -p $RDIR/boot
cat arch/arm/boot/zImage arch/arm/boot/dts/$DTBNAME >$RDIR/boot/$KNAME || exit 1

echo "dtbs..."
rm -rf $RDIR/boot/$DTBNAME
mkdir -p $RDIR/boot
cp -a arch/arm/boot/dts/$DTBNAME $RDIR/boot/ || exit 1

echo "output files:"
ls -al $RDIR/boot/* arch/arm/boot/Image

echo "done."

