#!/bin/sh
#
# Perform basic settings on a known IP camera
#
#
# Set custom upgrade url
#
fw_setenv upgrade 'https://github.com/OpenIPC/builder/releases/download/latest/ssc333_lite_tp-link-tapo-c110-v26-nor.tgz'
#
#
# Set custom majestic settings
#
cli -s .nightMode.irCutPin1 78
cli -s .nightMode.backlightPin 14
cli -s .nightMode.colorToGray true
cli -s .audio.speakerPin 61
cli -s .video0.codec h264
#
#
# Set wlan device and credentials if need
#
fw_setenv wlandev atbm6012b-ssc333-tapo-c110
#fw_setenv wlanssid Router
#fw_setenv wlanpass 12345678
#
# Ensure 8 MB SPI NOR partition layout with persistent rootfs_data overlay
#
fw_setenv rootmtd 5120k
fw_setenv bootargs 'console=ttyS0,115200 panic=20 root=/dev/mtdblock3 init=/init mtdparts=NOR_FLASH:256k(boot),64k(env),2048k(kernel),5120k(rootfs),-(rootfs_data) LX_MEM=${memlx} mma_heap=mma_heap_name0,miu=0,sz=${memsz} cma=2M'

exit 0
