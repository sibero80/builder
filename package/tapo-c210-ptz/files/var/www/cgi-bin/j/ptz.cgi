#!/bin/sh

. "$(dirname "$0")/../p/majestic.sh"

HORIZONTAL=""
VERTICAL=""
ACTION=""
PRESET=""
NAME=""
for param in $(echo "$QUERY_STRING" | tr '&' ' '); do
	case "$param" in
		h=*) HORIZONTAL="${param#*=}" ;;
		v=*) VERTICAL="${param#*=}" ;;
		act=*) ACTION="${param#*=}" ;;
		preset=*) PRESET="${param#*=}" ;;
		name=*) NAME="${param#*=}" ;;
	esac
done

# Allow GET for read-only JSON queries (act=presets, act=status)
if [ "$REQUEST_METHOD" = "GET" ]; then
	case "$ACTION" in
		presets)
			echo "HTTP/1.1 200 OK
Content-type: application/json; charset=UTF-8
Cache-Control: no-store

"
			/usr/sbin/ptz-controller presets
			exit $?
			;;
		status)
			echo "HTTP/1.1 200 OK
Content-type: application/json; charset=UTF-8
Cache-Control: no-store

"
			/usr/sbin/ptz-controller status
			exit $?
			;;
		*)
			echo "HTTP/1.1 405 Method Not Allowed
Content-type: text/plain; charset=UTF-8
Allow: GET, POST
Cache-Control: no-store

Use POST to move the camera, or GET with act=presets|status."
			exit 1
			;;
	esac
fi

if [ "$REQUEST_METHOD" != "POST" ]; then
	echo "HTTP/1.1 405 Method Not Allowed
Content-type: text/plain; charset=UTF-8
Allow: GET, POST
Cache-Control: no-store

Use POST to move the camera."
	exit 1
fi

cross_site=""
if [ -n "$HTTP_REFERER" ]; then
	ref_origin="${HTTP_REFERER#*://}"
	ref_origin="${ref_origin%%/*}"
	[ "$ref_origin" = "$HTTP_HOST" ] || cross_site=1
fi
case "$CONTENT_TYPE" in
	application/x-www-form-urlencoded*|multipart/form-data*|text/plain*)
		cross_site=1
		;;
esac
if [ -n "$cross_site" ]; then
	echo "HTTP/1.1 400 Bad Request
Content-type: text/plain; charset=UTF-8
Cache-Control: no-store

Cross-site PTZ refused."
	exit 1
fi

# Sanitize preset and name parameters
PRESET=$(echo "$PRESET" | tr -cd 'a-zA-Z0-9_-')
NAME=$(echo "$NAME" | tr -cd 'a-zA-Z0-9_-')

# Handle extended PTZ actions via /usr/sbin/ptz-controller
if [ -x /usr/sbin/ptz-controller ]; then
	case "$ACTION" in
		calibrate|recalibrate)
			echo "HTTP/1.1 200 OK
Content-type: application/json; charset=UTF-8
Cache-Control: no-store

"
			/usr/sbin/ptz-controller calibrate
			exit $?
			;;
		home)
			echo "HTTP/1.1 200 OK
Content-type: application/json; charset=UTF-8
Cache-Control: no-store

"
			/usr/sbin/ptz-controller goto 1
			exit $?
			;;
		goto)
			echo "HTTP/1.1 200 OK
Content-type: application/json; charset=UTF-8
Cache-Control: no-store

"
			/usr/sbin/ptz-controller goto "${PRESET:-1}"
			exit $?
			;;
		set)
			echo "HTTP/1.1 200 OK
Content-type: application/json; charset=UTF-8
Cache-Control: no-store

"
			/usr/sbin/ptz-controller set "${PRESET:-1}" "$NAME"
			exit $?
			;;
		remove)
			echo "HTTP/1.1 200 OK
Content-type: application/json; charset=UTF-8
Cache-Control: no-store

"
			/usr/sbin/ptz-controller remove "${PRESET:-1}"
			exit $?
			;;
		presets)
			echo "HTTP/1.1 200 OK
Content-type: application/json; charset=UTF-8
Cache-Control: no-store

"
			/usr/sbin/ptz-controller presets
			exit $?
			;;
		status)
			echo "HTTP/1.1 200 OK
Content-type: application/json; charset=UTF-8
Cache-Control: no-store

"
			/usr/sbin/ptz-controller status
			exit $?
			;;
		stop)
			echo "HTTP/1.1 200 OK
Content-type: text/plain; charset=UTF-8
Cache-Control: no-store

"
			/usr/sbin/ptz-controller stop
			exit $?
			;;
	esac
fi

echo "HTTP/1.1 200 OK
Content-type: text/plain; charset=UTF-8
Cache-Control: no-store
Pragma: no-cache

"

# WebUI Center button sends h=0&v=0 with empty act: return to Home preset (or calibrate if unhomed)
if [ -z "$ACTION" ] && [ "$HORIZONTAL" = "0" ] && [ "$VERTICAL" = "0" ] && [ -x /usr/sbin/ptz-controller ]; then
	/usr/sbin/ptz-controller goto 1 >/dev/null 2>&1
	exit $?
fi

echo "$HORIZONTAL" | grep -qE '^-?[0-9]{1,4}$' || HORIZONTAL=0
echo "$VERTICAL" | grep -qE '^-?[0-9]{1,4}$' || VERTICAL=0

ptz_control=$(fw_printenv -n ptz_control 2>/dev/null)

pelco_ok=0
pelco_why=""
gpio_ok=0
motor_ok=0
profile=""
case "$ptz_control" in
	pelco-d|pelco-xm|ms41908|gpiostep)
		mj_ptz > /dev/null 2>&1
		case $? in
			0) pelco_ok=1 ;;
			3) pelco_why="This camera has no PTZ driver installed: add the majestic-af package to its firmware." ;;
			1) pelco_why="This camera reports no motorized lens." ;;
			*) pelco_ok=1 ;;
		esac
		;;
	gpio)
		if command -v gpio-motors >/dev/null 2>&1 &&
			{ [ -n "$(fw_printenv -n ptz_gpio 2>/dev/null)" ] || [ -n "$(fw_printenv -n gpio_motors 2>/dev/null)" ]; }; then
			gpio_ok=1
		fi
		;;
	motor)
		profile=$(fw_printenv -n ptz_profile 2>/dev/null)
		[ -n "$profile" ] || profile=$(fw_printenv -n ptz 2>/dev/null)
		[ -x /usr/bin/motor ] && [ -n "$profile" ] && motor_ok=1
		;;
esac

ptz_caps=""
for cap in $(fw_printenv -n ptz_caps 2>/dev/null); do
	case "$cap" in
		pan|tilt|zoom|focus) ptz_caps="$ptz_caps $cap" ;;
	esac
done
[ -z "$ptz_caps" ] && [ "$ptz_control" = gpiostep ] && ptz_caps=" pan tilt"

has_cap() {
	[ -z "$ptz_caps" ] && return 0
	case " $ptz_caps " in *" $1 "*) return 0 ;; esac
	return 1
}

af_configured() {
	_af=$(mj_cfg ptz.enabled)
	case $? in
		0) [ "$_af" = "true" ] ;;
		1) return 1 ;;
		*) return 0 ;;
	esac
}

af_enabled() {
	{ [ "$pelco_ok" = 1 ] || [ "$gpio_ok" = 1 ] || [ "$motor_ok" = 1 ]; } &&
		has_cap focus &&
		af_configured
}

if [ -n "$ACTION" ]; then
	if [ "$ACTION" = "af" ]; then
		if ! af_enabled; then
			echo "Autofocus not available on this camera."
			exit 1
		fi
		r=$(curl -s -m 2 "http://127.0.0.1/autofocus")
		case "$r" in
			started|restarted|busy) ;;
			*)
				echo "Autofocus: engine did not answer."
				exit 1
				;;
		esac
		i=0
		s="running"
		while [ $i -lt 60 ]; do
			sleep 1
			s=$(curl -s -m 2 "http://127.0.0.1/autofocus/status")
			case "$s" in
				running|"") i=$((i + 1)) ;;
				*) break ;;
			esac
		done
		echo "Autofocus: ${s:-unknown}"
		case "$s" in
			done*) exit 0 ;;
			*) exit 1 ;;
		esac
	fi
	if [ "$pelco_ok" = 1 ]; then
		case "$ACTION" in
			up|down|left|right|stop|wide|tele|near|far)
				case "$ACTION" in
					wide|tele) has_cap zoom || ACTION="" ;;
					near|far) has_cap focus || ACTION="" ;;
					up|down) has_cap tilt || ACTION="" ;;
					left|right) has_cap pan || ACTION="" ;;
				esac
				if [ -z "$ACTION" ]; then
					echo "Not supported by this camera's PTZ."
					exit 1
				fi
				out=$(mj_ptz "$ACTION")
				rc=$?
				[ -n "$out" ] && echo "$out"
				exit $rc
				;;
		esac
		echo "Unknown PTZ action."
		exit 1
	fi
	echo "${pelco_why:-Pelco PTZ not available on this device.}"
	exit 1
fi

has_cap pan || HORIZONTAL=0
has_cap tilt || VERTICAL=0

if [ "$gpio_ok" = 1 ]; then
	gpio-motors "$HORIZONTAL" "$VERTICAL" 10
	exit $?
fi

if [ "$motor_ok" = 1 ]; then
	/usr/bin/motor "$profile" "$HORIZONTAL" "$VERTICAL"
	exit $?
fi

echo "PTZ not available on this device."
exit 1
