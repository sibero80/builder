################################################################################
#
# tapo-c210-ptz
#
################################################################################

TAPO_C210_PTZ_SITE_METHOD = local
TAPO_C210_PTZ_SITE = $(TAPO_C210_PTZ_PKGDIR)/src
TAPO_C210_PTZ_DEPENDENCIES = $(if $(BR2_PACKAGE_MAJESTIC_WEBUI),majestic-webui)

define TAPO_C210_PTZ_BUILD_CMDS
	$(MAKE) CC="$(TARGET_CC)" CFLAGS="$(TARGET_CFLAGS)" -C $(@D)
endef

define TAPO_C210_PTZ_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/ptz-controller $(TARGET_DIR)/usr/sbin/ptz-controller
	$(INSTALL) -D -m 0755 $(@D)/onvif_simple_server $(TARGET_DIR)/usr/sbin/onvif_simple_server
	$(INSTALL) -D -m 0755 $(@D)/majestic-af.so $(TARGET_DIR)/usr/lib/majestic-af.so
	mkdir -p $(TARGET_DIR)/usr/bin
	ln -sf /usr/sbin/ptz-controller $(TARGET_DIR)/usr/bin/gpio-motors
	cp -a $(TAPO_C210_PTZ_PKGDIR)/files/. $(TARGET_DIR)/
endef

$(eval $(generic-package))
