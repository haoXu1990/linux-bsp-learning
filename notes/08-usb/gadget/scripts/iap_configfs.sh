#!/bin/sh
#
# Configure a composite USB gadget for CarPlay iAP.
#
# Defaults create only the iAP function:
#   /sys/kernel/config/usb_gadget/g_carplay/functions/iap.carplay
#
# Optional composite functions can be enabled with environment variables:
#   ENABLE_ADB=1
#   ENABLE_ECM=1
#   ENABLE_RNDIS=1
#
# The f_iap kernel module must be installed in the target rootfs or provided
# through IAP_MODULE_PATH.

set -eu

GADGET_NAME="${GADGET_NAME:-g_carplay}"
CONFIG_NAME="${CONFIG_NAME:-c.1}"
CONFIG_LABEL="${CONFIG_LABEL:-CarPlay iAP}"
LANG="${LANG:-0x409}"

VID="${VID:-0x1234}"
PID="${PID:-0x5678}"
BCD_USB="${BCD_USB:-0x0200}"
BCD_DEVICE="${BCD_DEVICE:-0x0100}"
SERIAL="${SERIAL:-0123456789ABCDEF}"
MANUFACTURER="${MANUFACTURER:-zjinnova}"
PRODUCT="${PRODUCT:-CarPlay iAP Gadget}"

IAP_INSTANCE="${IAP_INSTANCE:-carplay}"
IAP_MODULE_PATH="${IAP_MODULE_PATH:-}"

ENABLE_ADB="${ENABLE_ADB:-0}"
ADB_INSTANCE="${ADB_INSTANCE:-adb}"
ADB_FFS_DIR="${ADB_FFS_DIR:-/dev/usb-ffs/adb}"
ADBD_BIN="${ADBD_BIN:-/bin/adbd}"
ADBD_PIDFILE="${ADBD_PIDFILE:-/var/run/adbd.pid}"

ENABLE_ECM="${ENABLE_ECM:-0}"
ENABLE_RNDIS="${ENABLE_RNDIS:-0}"
HOST_ADDR="${HOST_ADDR:-02:00:00:00:00:01}"
DEV_ADDR="${DEV_ADDR:-02:00:00:00:00:02}"

CONFIGFS_ROOT="${CONFIGFS_ROOT:-/sys/kernel/config}"
GADGET_ROOT="$CONFIGFS_ROOT/usb_gadget"
GADGET_DIR="$GADGET_ROOT/$GADGET_NAME"
CONFIG_DIR="$GADGET_DIR/configs/$CONFIG_NAME"

die()
{
	echo "iap_configfs: $*" >&2
	exit 1
}

info()
{
	echo "iap_configfs: $*"
}

ensure_configfs()
{
	modprobe libcomposite 2>/dev/null || true

	if ! mountpoint -q "$CONFIGFS_ROOT"; then
		mount -t configfs none "$CONFIGFS_ROOT"
	fi

	[ -d "$GADGET_ROOT" ] || die "$GADGET_ROOT does not exist"
}

load_iap_module()
{
	if [ -n "$IAP_MODULE_PATH" ]; then
		insmod "$IAP_MODULE_PATH" 2>/dev/null || true
	else
		modprobe f_iap 2>/dev/null || true
	fi
}

first_udc()
{
	if [ -n "${UDC:-}" ]; then
		echo "$UDC"
		return
	fi

	ls /sys/class/udc 2>/dev/null | sed -n '1p'
}

write_if_exists()
{
	file="$1"
	value="$2"

	if [ -e "$file" ]; then
		echo "$value" > "$file"
	fi
}

link_function()
{
	func="$1"
	name="$(basename "$func")"

	[ -e "$CONFIG_DIR/$name" ] || ln -s "$func" "$CONFIG_DIR/$name"
}

setup_gadget_base()
{
	mkdir -p "$GADGET_DIR"

	echo "$VID" > "$GADGET_DIR/idVendor"
	echo "$PID" > "$GADGET_DIR/idProduct"
	write_if_exists "$GADGET_DIR/bcdUSB" "$BCD_USB"
	write_if_exists "$GADGET_DIR/bcdDevice" "$BCD_DEVICE"

	mkdir -p "$GADGET_DIR/strings/$LANG"
	echo "$SERIAL" > "$GADGET_DIR/strings/$LANG/serialnumber"
	echo "$MANUFACTURER" > "$GADGET_DIR/strings/$LANG/manufacturer"
	echo "$PRODUCT" > "$GADGET_DIR/strings/$LANG/product"

	mkdir -p "$CONFIG_DIR"
	mkdir -p "$CONFIG_DIR/strings/$LANG"
	echo "$CONFIG_LABEL" > "$CONFIG_DIR/strings/$LANG/configuration"
}

setup_iap()
{
	func="$GADGET_DIR/functions/iap.$IAP_INSTANCE"

	mkdir -p "$func"
	link_function "$func"
}

setup_adb()
{
	func="$GADGET_DIR/functions/ffs.$ADB_INSTANCE"

	mkdir -p "$func"
	link_function "$func"

	mkdir -p "$ADB_FFS_DIR"
	if ! mountpoint -q "$ADB_FFS_DIR"; then
		mount -t functionfs "$ADB_INSTANCE" "$ADB_FFS_DIR"
	fi

	if [ -x "$ADBD_BIN" ]; then
		start-stop-daemon --start --oknodo --pidfile "$ADBD_PIDFILE" \
			--startas "$ADBD_BIN" --background 2>/dev/null || true
	else
		info "$ADBD_BIN not found; skipping adbd start"
	fi
}

setup_ecm()
{
	func="$GADGET_DIR/functions/ecm.usb0"

	mkdir -p "$func"
	write_if_exists "$func/host_addr" "$HOST_ADDR"
	write_if_exists "$func/dev_addr" "$DEV_ADDR"
	link_function "$func"
}

setup_rndis()
{
	func="$GADGET_DIR/functions/rndis.usb0"

	mkdir -p "$func"
	write_if_exists "$func/host_addr" "$HOST_ADDR"
	write_if_exists "$func/dev_addr" "$DEV_ADDR"
	link_function "$func"
}

bind_udc()
{
	udc="$(first_udc)"

	[ -n "$udc" ] || die "no UDC found under /sys/class/udc"
	echo "$udc" > "$GADGET_DIR/UDC"
	info "bound to UDC $udc"
}

unbind_udc()
{
	if [ -e "$GADGET_DIR/UDC" ]; then
		echo "" > "$GADGET_DIR/UDC" 2>/dev/null || true
	fi
}

remove_link()
{
	path="$1"

	if [ -L "$path" ]; then
		rm "$path"
	fi
}

stop_adb()
{
	start-stop-daemon --stop --oknodo --pidfile "$ADBD_PIDFILE" \
		--retry 5 2>/dev/null || true

	if mountpoint -q "$ADB_FFS_DIR"; then
		umount "$ADB_FFS_DIR" 2>/dev/null || true
	fi
}

stop_gadget()
{
	[ -d "$GADGET_DIR" ] || return 0

	unbind_udc
	stop_adb

	remove_link "$CONFIG_DIR/iap.$IAP_INSTANCE"
	remove_link "$CONFIG_DIR/ffs.$ADB_INSTANCE"
	remove_link "$CONFIG_DIR/ecm.usb0"
	remove_link "$CONFIG_DIR/rndis.usb0"

	rmdir "$GADGET_DIR/functions/iap.$IAP_INSTANCE" 2>/dev/null || true
	rmdir "$GADGET_DIR/functions/ffs.$ADB_INSTANCE" 2>/dev/null || true
	rmdir "$GADGET_DIR/functions/ecm.usb0" 2>/dev/null || true
	rmdir "$GADGET_DIR/functions/rndis.usb0" 2>/dev/null || true
	rmdir "$CONFIG_DIR/strings/$LANG" 2>/dev/null || true
	rmdir "$CONFIG_DIR" 2>/dev/null || true
	rmdir "$GADGET_DIR/strings/$LANG" 2>/dev/null || true
	rmdir "$GADGET_DIR" 2>/dev/null || true
}

start_gadget()
{
	ensure_configfs
	load_iap_module
	setup_gadget_base
	setup_iap

	if [ "$ENABLE_ADB" = "1" ]; then
		setup_adb
	fi

	if [ "$ENABLE_ECM" = "1" ]; then
		setup_ecm
	fi

	if [ "$ENABLE_RNDIS" = "1" ]; then
		setup_rndis
	fi

	bind_udc
}

case "${1:-start}" in
start)
	start_gadget
	;;
stop)
	stop_gadget
	;;
restart)
	stop_gadget
	start_gadget
	;;
status)
	if [ -e "$GADGET_DIR/UDC" ]; then
		echo "UDC=$(cat "$GADGET_DIR/UDC")"
	else
		echo "not configured"
	fi
	;;
*)
	echo "Usage: $0 {start|stop|restart|status}" >&2
	exit 2
	;;
esac
