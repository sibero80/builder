################################################################################
#
# openipc-motor
#
################################################################################

OPENIPC_MOTOR_VERSION = 1.0
OPENIPC_MOTOR_SITE_METHOD = local
OPENIPC_MOTOR_SITE = $(OPENIPC_MOTOR_PKGDIR)/src
OPENIPC_MOTOR_LICENSE = GPL-2.0

define OPENIPC_MOTOR_BUILD_CMDS
	$(TARGET_CC) $(TARGET_CFLAGS) -Os -s $(@D)/openipc-motor-ctl.c -o $(@D)/openipc-motor-ctl
endef

define OPENIPC_MOTOR_INSTALL_TARGET_CMDS
	$(INSTALL) -m 0755 -D $(@D)/openipc-motor-ctl $(TARGET_DIR)/usr/bin/openipc-motor-ctl
	mkdir -p $(TARGET_DIR)/etc/init.d
	$(INSTALL) -m 0755 -D $(OPENIPC_MOTOR_PKGDIR)/files/S95openipc-motor $(TARGET_DIR)/etc/init.d/S95openipc-motor
endef

$(eval $(kernel-module))
$(eval $(generic-package))
