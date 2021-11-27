#!/bin/sh

../n810/qemu/qemu-6.1-git/build/qemu-system-arm -M n810 -serial vc -serial vc -serial stdio -drive format=raw,if=sd,file=/dev/loop9419 -kernel arch/arm/boot/zImage -dtb arch/arm/boot/dts/omap2420-n810.dtb -append 'rootwait console=ttyO2,115200 earlyprintk debug ignore_loglevel omapfb.debug=y omapdss.debug=y dss.debug=y fbcon.debug=y console=tty1 omapfb.auto_update=y root=/dev/mmcblk0p2 rootfstype=ext4 ext4.debug=y mmc.debug=y ext4.mballoc_debug=1 jbd2.jbd2_debug=5 init=/bin/bash'

