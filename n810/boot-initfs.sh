#!/bin/bash

. `dirname $0`/init.sh

RDIR=${BDIR}/rootfs

#echo "flasher..."
flasher-3.5 -l -k ${RDIR}/boot/${KNAME} -n ${BDIR}/initfs.cpio.gz -b

echo "done."

