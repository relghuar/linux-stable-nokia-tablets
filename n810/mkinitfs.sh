#!/bin/bash

. `dirname $0`/init.sh

RDIR=${BDIR}/rootfs
IDIR=${BDIR}/initfs
IRDIR=${IDIR}/root

#MOD_LIST_FILE=${BDIR}/info/loaded-modules.txt
#MOD_LIST="`cat "${MOD_LIST_FILE}" | awk '{print $1}'`"

MOD_LIST="omap-aes-driver aes-arm aes_generic libaes omap-sham sha256-arm sha256_generic libsha256 ccm ctr omap-crypto crypto_engine libarc4 crc-ccitt omap-rng rng-core lzo_compress lzo_decompress zlib_deflate zlib_inflate nls_base nls_cp437 nls_iso8859-1 fat vfat jffs2 mtd_blkdevs mtdblock onenand onenand_omap2 ofpart omap-mailbox ledtrig-timer tsl2563 regmap-spi"

 echo """
./5.15.16-n810-00202-gdcf6004fd3af-dirty/kernel/arch/arm/crypto/aes-arm.ko
./5.15.16-n810-00202-gdcf6004fd3af-dirty/kernel/arch/arm/crypto/sha256-arm.ko
./5.15.16-n810-00202-gdcf6004fd3af-dirty/kernel/crypto/aes_generic.ko
./5.15.16-n810-00202-gdcf6004fd3af-dirty/kernel/crypto/ccm.ko
./5.15.16-n810-00202-gdcf6004fd3af-dirty/kernel/crypto/crypto_engine.ko
./5.15.16-n810-00202-gdcf6004fd3af-dirty/kernel/crypto/ctr.ko
./5.15.16-n810-00202-gdcf6004fd3af-dirty/kernel/crypto/sha256_generic.ko
./5.15.16-n810-00202-gdcf6004fd3af-dirty/kernel/drivers/base/regmap/regmap-spi.ko
./5.15.16-n810-00202-gdcf6004fd3af-dirty/kernel/drivers/char/hw_random/omap-rng.ko
./5.15.16-n810-00202-gdcf6004fd3af-dirty/kernel/drivers/char/hw_random/rng-core.ko
./5.15.16-n810-00202-gdcf6004fd3af-dirty/kernel/drivers/crypto/omap-aes-driver.ko
./5.15.16-n810-00202-gdcf6004fd3af-dirty/kernel/drivers/crypto/omap-crypto.ko
./5.15.16-n810-00202-gdcf6004fd3af-dirty/kernel/drivers/crypto/omap-sham.ko
./5.15.16-n810-00202-gdcf6004fd3af-dirty/kernel/drivers/iio/light/tsl2563.ko
./5.15.16-n810-00202-gdcf6004fd3af-dirty/kernel/drivers/input/evdev.ko
./5.15.16-n810-00202-gdcf6004fd3af-dirty/kernel/drivers/input/mousedev.ko
./5.15.16-n810-00202-gdcf6004fd3af-dirty/kernel/drivers/input/touchscreen/tsc2005.ko
./5.15.16-n810-00202-gdcf6004fd3af-dirty/kernel/drivers/input/touchscreen/tsc200x-core.ko
./5.15.16-n810-00202-gdcf6004fd3af-dirty/kernel/drivers/leds/trigger/ledtrig-timer.ko
./5.15.16-n810-00202-gdcf6004fd3af-dirty/kernel/drivers/mailbox/omap-mailbox.ko
./5.15.16-n810-00202-gdcf6004fd3af-dirty/kernel/drivers/mtd/mtd_blkdevs.ko
./5.15.16-n810-00202-gdcf6004fd3af-dirty/kernel/drivers/mtd/mtdblock.ko
./5.15.16-n810-00202-gdcf6004fd3af-dirty/kernel/drivers/mtd/nand/onenand/onenand.ko
./5.15.16-n810-00202-gdcf6004fd3af-dirty/kernel/drivers/mtd/nand/onenand/onenand_omap2.ko
./5.15.16-n810-00202-gdcf6004fd3af-dirty/kernel/drivers/mtd/parsers/ofpart.ko
./5.15.16-n810-00202-gdcf6004fd3af-dirty/kernel/fs/autofs/autofs4.ko
./5.15.16-n810-00202-gdcf6004fd3af-dirty/kernel/fs/configfs/configfs.ko
./5.15.16-n810-00202-gdcf6004fd3af-dirty/kernel/fs/fat/fat.ko
./5.15.16-n810-00202-gdcf6004fd3af-dirty/kernel/fs/fat/vfat.ko
./5.15.16-n810-00202-gdcf6004fd3af-dirty/kernel/fs/jffs2/jffs2.ko
./5.15.16-n810-00202-gdcf6004fd3af-dirty/kernel/fs/nls/nls_base.ko
./5.15.16-n810-00202-gdcf6004fd3af-dirty/kernel/fs/nls/nls_cp437.ko
./5.15.16-n810-00202-gdcf6004fd3af-dirty/kernel/fs/nls/nls_iso8859-1.ko
./5.15.16-n810-00202-gdcf6004fd3af-dirty/kernel/lib/crc-ccitt.ko
./5.15.16-n810-00202-gdcf6004fd3af-dirty/kernel/lib/crypto/libaes.ko
./5.15.16-n810-00202-gdcf6004fd3af-dirty/kernel/lib/crypto/libarc4.ko
./5.15.16-n810-00202-gdcf6004fd3af-dirty/kernel/lib/crypto/libsha256.ko
./5.15.16-n810-00202-gdcf6004fd3af-dirty/kernel/lib/lzo/lzo_compress.ko
./5.15.16-n810-00202-gdcf6004fd3af-dirty/kernel/lib/lzo/lzo_decompress.ko
./5.15.16-n810-00202-gdcf6004fd3af-dirty/kernel/lib/zlib_deflate/zlib_deflate.ko
./5.15.16-n810-00202-gdcf6004fd3af-dirty/kernel/lib/zlib_inflate/zlib_inflate.ko
""" >/dev/null

echo "Copying modules..."
rm -rf "${IRDIR}/lib/modules"
mkdir -p "${IRDIR}/lib/modules"

pushd "${RDIR}/lib/modules" >/dev/null
modfiles="`
for f in ${MOD_LIST} ; do
	fm="${f//_/?}"
	find -name "${fm}.ko"
done
`"
popd >/dev/null

for mf in $modfiles ; do
	#echo $mf
	mkdir -p "`dirname "${IRDIR}/lib/modules/${mf}"`"
	cp -a "${RDIR}/lib/modules/${mf}" "${IRDIR}/lib/modules/${mf}"
done
cp -a "${RDIR}/lib/modules"/*/modules.* "${IRDIR}/lib/modules"/*/

echo "Creating initramfs..."
(cd "${IRDIR}"; find . | cpio -o -H newc | gzip) > "${BDIR}/initfs.cpio.gz"

