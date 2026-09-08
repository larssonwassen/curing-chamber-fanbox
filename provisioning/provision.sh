#!/usr/bin/env bash
#
# Build an NVS image from secrets.csv and write it to the device's nvs
# partition. Run from anywhere; paths are resolved relative to this script.
#
#   ./provisioning/provision.sh /dev/cu.usbmodem3101
#
# Requires an activated ESP-IDF environment (IDF_PATH set). Only the nvs
# partition is touched -- the application is left alone, so re-provisioning a
# deployed device does not mean reflashing it.

set -euo pipefail

PORT="${1:-}"
if [ -z "$PORT" ]; then
	echo "usage: $0 <serial-port>" >&2
	echo "  e.g. $0 /dev/cu.usbmodem3101" >&2
	exit 2
fi

if [ -z "${IDF_PATH:-}" ]; then
	echo "IDF_PATH is not set. Activate the ESP-IDF environment first." >&2
	exit 2
fi

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(dirname "$HERE")"
CSV="$HERE/secrets.csv"
BIN="$HERE/nvs.bin"

if [ ! -f "$CSV" ]; then
	echo "$CSV not found. Copy secrets.csv.example to secrets.csv and fill it in." >&2
	exit 1
fi

# Read the offset and size from the partition table rather than repeating them,
# so this cannot drift out of step with partitions.csv.
read -r OFFSET SIZE < <(
	awk -F',' '
		{ gsub(/[[:space:]]/, "", $1) }
		$1 == "nvs" {
			gsub(/[[:space:]]/, "", $4)
			gsub(/[[:space:]]/, "", $5)
			print $4, $5
			exit
		}
	' "$ROOT/partitions.csv"
)

if [ -z "${OFFSET:-}" ] || [ -z "${SIZE:-}" ]; then
	echo "Could not find the nvs partition in $ROOT/partitions.csv" >&2
	exit 1
fi

echo "nvs partition: offset $OFFSET size $SIZE"

python "$IDF_PATH/components/nvs_flash/nvs_partition_generator/nvs_partition_gen.py" \
	generate "$CSV" "$BIN" "$SIZE"

python -m esptool --chip esp32s3 -p "$PORT" write-flash "$OFFSET" "$BIN"

echo
echo "Provisioned. The device reads these at boot; reset it to pick them up."
