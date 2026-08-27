#!/usr/bin/env bash
# Flash a prebuilt HiveInside factory image onto a XIAO nRF54LM20A Sense over
# the board's on-board CMSIS-DAP debugger, using OpenOCD only — no Zephyr
# toolchain and no west workspace required.
#
# It runs the same OpenOCD sequence `west flash --verify` runs for this board,
# with one difference that matters: the board's RRAM loader is corrected first
# (see rram_write_buffer_fix below), because the upstream one silently drops
# the tail of any image whose length is not a multiple of 16 bytes.
#
# Usage:  ./flash.sh [options] [image.hex]
# See README.md next to this script, or docs/flashing.md in the repository.

set -euo pipefail

script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
openocd_bin=${OPENOCD:-openocd}
openocd_cfg=${HIVEINSIDE_OPENOCD_CFG:-}
image=""
verify=1

die() { printf 'error: %s\n' "$*" >&2; exit 1; }

usage() {
	sed -n '2,13p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'
	cat <<'USAGE'

Options:
  --openocd-cfg PATH  Board OpenOCD config to use. Defaults to the copy in
                      openocd/ next to this script, or to the one in a west
                      workspace found from the current directory.
  --openocd PATH      OpenOCD binary (default: openocd, or $OPENOCD).
  --no-verify         Skip the read-back verification. Not recommended.
  -h, --help          This text.
USAGE
}

while [ $# -gt 0 ]; do
	case "$1" in
	--openocd-cfg) openocd_cfg=${2:-}; shift 2 ;;
	--openocd) openocd_bin=${2:-}; shift 2 ;;
	--no-verify) verify=0; shift ;;
	-h|--help) usage; exit 0 ;;
	-*) die "unknown option: $1 (try --help)" ;;
	*) [ -z "$image" ] || die "more than one image given"; image=$1; shift ;;
	esac
done

# ── The image ──────────────────────────────────────────────────────────────
#
# Only the factory hex is flashable over SWD. The .signed.bin next to it is the
# BLE OTA payload: it is the application alone, so flashing it leaves nothing
# at 0x0 and the device goes completely silent.
if [ -z "$image" ]; then
	candidates=()
	for dir in "$PWD" "$script_dir"; do
		while IFS= read -r -d '' f; do candidates+=("$f"); done \
			< <(find "$dir" -maxdepth 1 -name 'hiveinside-*-factory.hex' -print0 2>/dev/null)
		[ ${#candidates[@]} -eq 0 ] || break
	done
	case ${#candidates[@]} in
	0) die "no hiveinside-*-factory.hex found here; pass one as an argument" ;;
	1) image=${candidates[0]} ;;
	*) die "several factory images found; name the one to flash:
$(printf '  %s\n' "${candidates[@]}")" ;;
	esac
fi

case "$image" in
*.signed.bin)
	die "$image is the BLE OTA payload, not an SWD image.
Flash hiveinside-<version>-<variant>-factory.hex instead; upload the .signed.bin
through HiveHub." ;;
*.hex) ;;
*) die "expected a .hex factory image, got: $image" ;;
esac
[ -f "$image" ] || die "no such file: $image"

command -v "$openocd_bin" >/dev/null 2>&1 \
	|| die "openocd not found (looked for '$openocd_bin').
Install it: apt install openocd | brew install open-ocd | pacman -S openocd,
or point at one with --openocd /path/to/openocd."

# ── The board config ───────────────────────────────────────────────────────
if [ -z "$openocd_cfg" ]; then
	if [ -f "$script_dir/openocd/xiao_nrf54lm20a.cfg" ]; then
		openocd_cfg="$script_dir/openocd/xiao_nrf54lm20a.cfg"
	elif topdir=$(west topdir 2>/dev/null); then
		openocd_cfg="$topdir/zephyr/boards/seeed/xiao_nrf54lm20a/support/openocd.cfg"
	fi
fi
[ -n "$openocd_cfg" ] && [ -f "$openocd_cfg" ] \
	|| die "no board OpenOCD config found; pass one with --openocd-cfg.
It is boards/seeed/xiao_nrf54lm20a/support/openocd.cfg in a Zephyr checkout,
and ships in openocd/ inside the release's hiveinside-flash-tools.zip."

workdir=$(mktemp -d)
trap 'rm -rf "$workdir"' EXIT

# ── rram_write_buffer_fix ──────────────────────────────────────────────────
#
# The upstream loader is:
#
#     proc nrf54lm20a-load {file} {
#         mww 0x5004e500 0x101
#         load_image $file
#     }
#
# 0x5004e500 is RRAMC.CONFIG; 0x101 sets WEN=1 *and a one-line (16-byte) write
# buffer*, and the proc never commits that buffer. The nRF54L RRAM controller
# only writes a 128-bit line out once the line fills, so whatever does not
# reach a 16-byte boundary stays in the buffer and never lands in RRAM. On a
# signed image that silently truncates the signature TLV, and MCUboot then
# reports `E: Image in the primary slot is not valid!`.
#
# 0x1 is the same register with the write buffer disabled: every write commits.
# Slower, and correct.
cfg="$workdir/board.cfg"
if grep -q '0x5004e500 0x101' "$openocd_cfg"; then
	sed 's/mww 0x5004e500 0x101/mww 0x5004e500 0x1/' "$openocd_cfg" > "$cfg"
	printf 'Applied the RRAM write-buffer fix to a temporary copy of the board config.\n'
elif grep -q '0x5004e500 0x1' "$openocd_cfg"; then
	cp "$openocd_cfg" "$cfg"
	printf 'Board config already commits every RRAM write; using it unchanged.\n'
else
	die "$openocd_cfg has no recognisable nrf54lm20a-load RRAM setup.
Upstream may have restructured it — check it by hand before flashing."
fi

# OpenOCD passes the file name through Tcl, which splits on whitespace.
if printf '%s' "$image" | grep -q '[[:space:]]'; then
	cp "$image" "$workdir/image.hex"
	image="$workdir/image.hex"
fi

verify_args=()
if [ "$verify" -eq 1 ]; then
	# Read the image back. This is the check that catches the truncation the
	# fix above prevents, and any half-written flash.
	verify_args=(-c "reset init" -c "verify_image $image")
fi

printf 'Flashing %s\n' "$image"
"$openocd_bin" -f "$cfg" \
	-c "init" \
	-c "targets nrf54lm20a.cpu" \
	-c "reset init" \
	-c "nrf54lm20a-load $image" \
	"${verify_args[@]}" \
	-c "reset run" \
	-c "shutdown"

cat <<'DONE'

Done. The node reboots into MCUboot and then the application.

A bringup image prints its banner on the board's USB serial port at 115200 8N1
(/dev/ttyACM0 on Linux, /dev/cu.usbmodem* on macOS) — press RST with the monitor
attached to see it. A lowpower image has no console by design; look for its
BLE advertisement (HiveInside-XXXX) instead.
DONE
