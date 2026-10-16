CC ?= gcc
CFLAGS ?= -Os
EXTRA_CFLAGS = -Os -s -Wall -Wextra -ffunction-sections -fdata-sections
LDFLAGS = -Wl,--gc-sections

all: ptz-controller majestic-af.so onvif_simple_server

ptz-controller: ptz-controller.c
	$(CC) $(CFLAGS) $(EXTRA_CFLAGS) $(LDFLAGS) -o $@ $<

majestic-af.so: majestic_af_ptz.c
	$(CC) $(CFLAGS) $(EXTRA_CFLAGS) -fPIC -shared $(LDFLAGS) -o $@ $<

onvif_simple_server: onvif_simple_server.c
	$(CC) $(CFLAGS) $(EXTRA_CFLAGS) $(LDFLAGS) -o $@ $<

clean:
	rm -f ptz-controller majestic-af.so onvif_simple_server

.PHONY: all clean
