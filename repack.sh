#!/bin/sh
set -e

openipc=https://github.com/openipc/firmware/releases/download/latest
builder=https://github.com/openipc/builder/releases/download/latest

if ! command -v mksquashfs >/dev/null 2>&1 || ! command -v unsquashfs >/dev/null 2>&1; then
	echo "Missing required squashfs tools. Please install: sudo apt-get install squashfs-tools"
	exit 1
fi

LOCAL_BUILD=false
ARGS=""
for arg in "$@"; do
	case "$arg" in
		-l|--local)
			LOCAL_BUILD=true
			;;
		*)
			ARGS="$ARGS \"$arg\""
			;;
	esac
done

eval set -- "$ARGS"

if [ -z "$1" ] || [ -z "$2" ]; then
	echo "Usage: $0 [-l|--local] [uboot] [firmware] [ssid] [pass] [flash_size_kb]"
	echo ""
	echo "Options:"
	echo "  -l, --local   Use locally built firmware archive from archive/ or openipc/output/images/"
	echo ""
	echo "Arguments:"
	echo "  [uboot]       U-Boot platform name (e.g. ssc333) or path to u-boot binary"
	echo "  [firmware]    Device name (e.g. ssc333_lite_tp-link-tapo-c210-v2) or path to .tgz"
	echo "  [ssid]        (Optional) Wi-Fi network SSID"
	echo "  [pass]        (Optional) Wi-Fi network password"
	echo "  [flash_size]  (Optional) Target flash size in KB (default: 8192 for NOR / 8MB)"
	echo ""
	echo "Examples:"
	echo "  $0 ssc333 ssc333_lite_tp-link-tapo-c110-v2 MySSID MyPass"
	echo "  $0 -l ssc333 ssc333_lite_tp-link-tapo-c210-v2 MySSID MyPass"
	exit 1
fi

uboot_target="$1"
firmware_target="$2"
ssid="$3"
pass="$4"
flash_size="${5:-8192}"

# Determine chipset from firmware name or file
chipset=$(basename "$firmware_target" | cut -d_ -f1 | sed 's/^openipc\.//; s/-nor.*//')
release="openipc-${uboot_target}-nor.bin"

mkdir -p output

# 1. Obtain U-Boot
if [ -f "$uboot_target" ]; then
	echo "Using local U-Boot binary: $uboot_target"
	cp -f "$uboot_target" output/uboot.bin
elif [ -f "openipc/output/images/u-boot-${uboot_target}-nor.bin" ]; then
	echo "Using local U-Boot from openipc/output/images"
	cp -f "openipc/output/images/u-boot-${uboot_target}-nor.bin" output/uboot.bin
else
	uboot_file="u-boot-${uboot_target}-nor.bin"
	echo "Downloading U-Boot: $uboot_file"
	if ! wget -q --show-progress "$openipc/$uboot_file" -O output/uboot.bin; then
		echo "Download failed: $openipc/$uboot_file"
		exit 1
	fi
fi

# 2. Obtain Firmware (Local vs Remote)
if [ "$LOCAL_BUILD" = "true" ]; then
	echo "Looking for local build for $firmware_target..."
	if [ -f "$firmware_target" ]; then
		echo "Using local archive: $firmware_target"
		tar -xf "$firmware_target" -C output
	else
		# Search in archive/<device>/ (newest timestamp)
		latest_archive=$(ls -td archive/"${firmware_target}"/*/ 2>/dev/null | head -n1)
		if [ -d "$latest_archive" ]; then
			archive_file=$(ls -t "${latest_archive}"openipc.*.tgz 2>/dev/null | head -n1)
			if [ -f "$archive_file" ]; then
				echo "Using local archive: $archive_file"
				tar -xf "$archive_file" -C output
			else
				echo "Copying images directly from $latest_archive"
				cp -a "$latest_archive"* output/
			fi
		elif [ -d "openipc/output/images" ]; then
			echo "Using local images from openipc/output/images"
			cp -a openipc/output/images/uImage* output/ 2>/dev/null || true
			cp -a openipc/output/images/rootfs.squashfs* output/ 2>/dev/null || true
		else
			echo "Error: No local build artifacts found for $firmware_target in archive/ or openipc/output/images"
			exit 1
		fi
	fi
else
	echo "Downloading published firmware: $firmware_target"
	fw_archive="$firmware_target"
	case "$fw_archive" in
		*.tgz) ;;
		*-nor) fw_archive="${fw_archive}.tgz" ;;
		*) fw_archive="${fw_archive}-nor.tgz" ;;
	esac

	if ! wget -q --show-progress "$builder/$fw_archive" -O output/builder.tgz; then
		if ! wget -q --show-progress "$builder/$firmware_target.tgz" -O output/builder.tgz; then
			echo "Download failed: $builder/$fw_archive"
			exit 1
		fi
	fi
	tar -xf output/builder.tgz -C output
fi

# Normalize image filenames if needed
if [ ! -f "output/uImage.$chipset" ] && [ -f "output/uImage" ]; then
	cp -f output/uImage "output/uImage.$chipset"
fi
if [ ! -f "output/rootfs.squashfs.$chipset" ] && [ -f "output/rootfs.squashfs" ]; then
	cp -f output/rootfs.squashfs "output/rootfs.squashfs.$chipset"
fi

if [ ! -f "output/uImage.$chipset" ]; then
	echo "Error: output/uImage.$chipset not found!"
	exit 1
fi
if [ ! -f "output/rootfs.squashfs.$chipset" ]; then
	echo "Error: output/rootfs.squashfs.$chipset not found!"
	exit 1
fi

# 3. Bake Wi-Fi Credentials (if provided)
if [ -n "$ssid" ] && [ -n "$pass" ]; then
	echo "Baking Wi-Fi credentials for SSID: $ssid"
	unsquashfs -d output/squashfs "output/rootfs.squashfs.$chipset" > /dev/null
	file=output/squashfs/usr/share/openipc/wireless.sh
	mkdir -p $(dirname "$file")
	cat << EOF > "$file"
#!/bin/sh
fw_setenv wlanssid "$ssid"
fw_setenv wlanpass "$pass"
EOF
	chmod 755 "$file"
	rm -f "output/rootfs.$chipset"
	mksquashfs output/squashfs "output/rootfs.$chipset" -comp xz > /dev/null
	rm -rf output/squashfs
else
	mv "output/rootfs.squashfs.$chipset" "output/rootfs.$chipset"
fi

# Check rootfs size limit
rootfs_bytes=$(stat -c%s "output/rootfs.$chipset")
if [ "$flash_size" -le 8192 ] && [ "$rootfs_bytes" -gt 5242880 ]; then
	echo "Error: Rootfs ($rootfs_bytes bytes) exceeds 5120 KB partition limit (5,242,880 bytes)!"
	exit 1
fi

# 4. Generate flashrom-ready 8MB (or specified flash_size) NOR binary padded with 0xFF
echo "Assembling $flash_size KB flashrom-ready binary: $release"
tr '\000' '\377' < /dev/zero | dd of="$release" bs=1K count="$flash_size" status=none
dd if=output/uboot.bin of="$release" bs=1K seek=0 conv=notrunc status=none
dd if="output/uImage.$chipset" of="$release" bs=1K seek=320 conv=notrunc status=none
dd if="output/rootfs.$chipset" of="$release" bs=1K seek=2368 conv=notrunc status=none
rm -rf output

total_bytes=$(stat -c%s "$release")
echo "Created: $release ($total_bytes bytes)"
