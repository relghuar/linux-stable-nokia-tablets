
PATH=/home/nightwolf/hardware/nokia/n810/toolchain/gcc-linaro-7.5.0-2019.12-x86_64_arm-linux-gnueabihf/bin:$PATH
ARCH=arm
CROSS_COMPILE=arm-linux-gnueabihf-

GCC="`which ${CROSS_COMPILE}gcc`"
if [ ! -f "$GCC" ] ; then
	echo "ERROR: no $GCC found!"
	exit 1
fi
GCCVER="`$GCC --version | head -1`"

BDIR="`dirname $0`"
DTBNAME=omap2420-n810.dtb

KVER="`cat Makefile 2>/dev/null | awk '/^VERSION =/{v=$3} /^PATCHLEVEL =/{p=$3} /^SUBLEVEL =/{s=$3} END{print v "." p "." s}' 2>/dev/null`"

if [ ! -f Makefile -o "$KVER" == ".." ] ; then
	echo "ERROR: no kernel found!"
	exit 1
fi

KNAME=zImage-${KVER}

echo "Building ${arch} kernel $KVER using ${CROSS_COMPILE} ($GCC :: $GCCVER)"

