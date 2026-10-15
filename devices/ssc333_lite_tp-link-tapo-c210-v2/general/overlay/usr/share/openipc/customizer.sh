#!/bin/sh
#
# Perform basic settings on a known IP camera
#
#
# Set custom upgrade url
#
fw_setenv upgrade 'https://github.com/OpenIPC/builder/releases/download/latest/ssc333_lite_tp-link-tapo-c210-v2-nor.tgz'
#
#
# Set custom majestic settings
#
cli -s .isp.sensorConfig /etc/sensors/sc3338.bin
cli -s .nightMode.irCutPin1 78
cli -s .nightMode.backlightPin 14
cli -s .nightMode.colorToGray true
cli -s .audio.speakerPin 61
cli -s .video0.codec h264
#
#
# Set PTZ motor settings
#
fw_setenv ptz_control gpio
fw_setenv ptz_gpio '44 45 46 47 80 79 76 77'
fw_setenv ptz_caps 'pan tilt'
#
#
# Set wlan device and sensor
#
fw_setenv wlandev ssw101b-ssc333-tapo-c110
fw_setenv sensor sc3338
#fw_setenv wlanssid Router
#fw_setenv wlanpass 12345678

exit 0
