# tapo-c210-ptz

Buildroot package providing unified PTZ motion control, quadratic trapezoidal velocity ramping, soft-touch calibration, persistent coordinate/preset tracking, and native ONVIF Profile S/T PTZ preset compatibility for the TP-Link Tapo C210 v2 / C220 (SigmaStar SSC333).

## Installed Components

| Path | Description |
| :--- | :--- |
| `/usr/sbin/ptz-controller` | C motion & preset engine (`step`, `abs`, `move`, `stop`, `goto`, `set`, `remove`, `presets`, `calibrate`, `boot`, `status`) |
| `/usr/bin/gpio-motors` | Symlink to `/usr/sbin/ptz-controller` for drop-in `gpio-motors` compatibility |
| `/usr/lib/majestic-af.so` | Majestic PTZ plugin enabling Port 80 ONVIF PTZ + Preset SOAP actions (`GetPresets`, `GotoPreset`, `SetPreset`, `RemovePreset`, `GotoHomePosition`, `RelativeMove`, `AbsoluteMove`) |
| `/usr/sbin/onvif_simple_server` | Standalone ONVIF PTZ SOAP daemon (Port 8080) driven by `/etc/onvif_simple_server.conf` |
| `/etc/init.d/S96onvif` | Restores saved coordinates on boot (`ptz-controller boot`) and starts `onvif_simple_server` |
| `/etc/ptz_presets.json` | Persistent flash storage for homed status, coordinates, and named presets |
| `/var/www/cgi-bin/j/ptz.cgi` | Extended OpenIPC WebUI & HTTP CGI handler (`act=calibrate`, `act=goto`, `act=set`, `act=remove`, `act=presets`, `act=status`, D-pad & Center) |
