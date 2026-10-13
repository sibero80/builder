#!/bin/sh
openipc=https://github.com/openipc/firmware/releases/download/latest
builder=https://github.com/openipc/builder/releases/download/latest

if ! command -v mksquashfs >/dev/null 2>&1 || ! command -v unsquashfs >/dev/null 2>&1; then
	echo "Missing required squashfs tools. Please install: sudo apt-get install squashfs-tools"
	exit 1
fi

if [ -z "$1" ]; then
	echo "Usage: $0 [uboot] [firmware] [ssid] [pass] [flash_size_kb]"
	echo "Example: $0 ssc333 ssc333_lite_tp-link-tapo-c110-v2 MySSID MyPass"
	exit 0
fi

uboot=u-boot-$1-nor.bin
chipset=$(echo $2 | cut -d_ -f1)
release=openipc-$1-nor.bin

# Target flash size in KB (default: 8192 KB / 8 MB for SPI NOR chips)
flash_size=8192
if echo "$2" | grep -qi -E "16mb|16m|ultimate"; then
	flash_size=16384
fi
if [ -n "$5" ]; then
	flash_size=$5
fi

mkdir -p output
if ! wget -q --show-progress $openipc/$uboot -O output/uboot.bin; then
	echo "Download failed: $openipc/$uboot"
	exit 1
fi

# Download firmware archive (handle with or without -nor / .tgz suffix)
fw_archive="$2"
case "$fw_archive" in
	*.tgz) ;;
	*-nor) fw_archive="${fw_archive}.tgz" ;;
	*) fw_archive="${fw_archive}-nor.tgz" ;;
esac

if ! wget -q --show-progress $builder/$fw_archive -O output/builder.tgz; then
	if ! wget -q --show-progress $builder/$2.tgz -O output/builder.tgz; then
		echo "Download failed: $builder/$fw_archive"
		exit 1
	fi
fi

tar -xf output/builder.tgz -C output

needs_resquash=false
if [ -n "$3" ] && [ -n "$4" ]; then
	needs_resquash=true
fi
if [ "$chipset" = "ssc333" ] || [ "$flash_size" -le 8192 ]; then
	needs_resquash=true
fi

if $needs_resquash; then
	unsquashfs -d output/squashfs output/rootfs.squashfs.$chipset > /dev/null

	# Strip uncompressed PCM audio on 8 MB builds to stay safely below U-Boot's 5.238 MB rootmtd threshold
	rm -f output/squashfs/usr/share/openipc/sounds/ready_48k.pcm

	if [ -n "$3" ] && [ -n "$4" ]; then
		file=output/squashfs/usr/share/openipc/wireless.sh
		echo "#!/bin/sh" >> $file
		echo "fw_setenv wlanssid $3" >> $file
		echo "fw_setenv wlanpass $4" >> $file
		echo "fw_setenv rootmtd 5120k" >> $file
		echo "fw_setenv bootargs 'console=ttyS0,115200 panic=20 root=/dev/mtdblock3 init=/init mtdparts=NOR_FLASH:256k(boot),64k(env),2048k(kernel),5120k(rootfs),-(rootfs_data) LX_MEM=\${memlx} mma_heap=mma_heap_name0,miu=0,sz=\${memsz} cma=2M'" >> $file
		chmod 755 $file
	fi

	mksquashfs output/squashfs output/rootfs.$chipset -comp xz > /dev/null
else
	mv output/rootfs.squashfs.$chipset output/rootfs.$chipset
fi

rootfs_bytes=$(stat -c%s output/rootfs.$chipset)
if [ "$flash_size" -le 8192 ] && [ "$rootfs_bytes" -gt 5242880 ]; then
	echo "ERROR: Rootfs ($rootfs_bytes bytes) exceeds 5120 KB partition limit (5242880 bytes)!"
	exit 1
fi

# Pre-fill full flash size with 0xFF (erased NOR state) so output is ready for flashrom/programmers
dd if=/dev/zero bs=1K count=$flash_size status=none | tr '\000' '\377' > $release
dd if=output/uboot.bin of=$release bs=1K seek=0 conv=notrunc status=none
dd if=output/uImage.$chipset of=$release bs=1K seek=320 conv=notrunc status=none
dd if=output/rootfs.$chipset of=$release bs=1K seek=2368 conv=notrunc status=none
rm -rf output

echo "Created: $release ($(stat -c%s $release) bytes - $(($flash_size / 1024)) MB flashrom-ready)"
