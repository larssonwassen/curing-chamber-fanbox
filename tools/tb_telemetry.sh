#!/usr/bin/env bash
#
# Read telemetry and attributes for the chamber out of ThingsBoard.
#
#   ./tools/tb_telemetry.sh latest
#   ./tools/tb_telemetry.sh series 24            # last 24 h, every reading
#   ./tools/tb_telemetry.sh series 72 900        # last 72 h, 15-minute averages
#   ./tools/tb_telemetry.sh attrs
#   ./tools/tb_telemetry.sh csv 48 > chamber.csv
#
# Read-only: nothing here writes to ThingsBoard or to the device.
#
# The credential is never passed on the command line and never printed. It is
# read from a file, so it does not sit in shell history, in the process table
# where any other user on the machine can see it, or in a terminal someone is
# looking over. Create it once:
#
#   mkdir -p ~/.config/curing-chamber
#   printf '%s' 'tb_api_key_here' > ~/.config/curing-chamber/tb_api_key
#   chmod 600 ~/.config/curing-chamber/tb_api_key
#
# A key created under a ThingsBoard user carries that user's permissions, so
# prefer a key that only needs to read. This script is also worth pointing at a
# separate key from the one CI uses, so either can be revoked without breaking
# the other.
#
# Override the defaults with TB_URL and TB_DEVICE_NAME if they are wrong.

set -euo pipefail

TB_URL="${TB_URL:-https://tb.itviking.se}"
TB_DEVICE_NAME="${TB_DEVICE_NAME:-Curing Chamber Fanbox}"
KEY_FILE="${TB_KEY_FILE:-$HOME/.config/curing-chamber/tb_api_key}"

# Everything the firmware publishes as timeseries. plate_temperature is null
# whenever a probe is configured but its reading has gone stale, which is
# deliberate -- a gap should look like a gap.
KEYS="temperature,humidity,plate_temperature,fan_duty_cycle,fan_rpm"

usage() {
	cat >&2 <<EOF
usage: $0 <command> [args]

  latest                 most recent value of every telemetry key
  series [HOURS] [AGGS]  timeseries; HOURS default 24, AGGS is an averaging
                         interval in seconds (omit for raw readings)
  csv [HOURS] [AGGS]     the same, as CSV on stdout
  attrs                  client and shared attributes, including vent_state
  keys                   which telemetry keys this device has ever published
EOF
	exit 2
}

if [ ! -r "$KEY_FILE" ]; then
	cat >&2 <<EOF
No ThingsBoard key at $KEY_FILE

Create one in ThingsBoard under your user's profile, then:

  mkdir -p "$(dirname "$KEY_FILE")"
  printf '%s' 'the-key' > "$KEY_FILE"
  chmod 600 "$KEY_FILE"
EOF
	exit 1
fi

# Read into a variable rather than interpolating the file anywhere a process
# listing or an error message could carry it.
TB_API_KEY="$(cat "$KEY_FILE")"
TB_API_KEY="${TB_API_KEY%%[[:space:]]}"
if [ -z "$TB_API_KEY" ]; then
	echo "$KEY_FILE is empty" >&2
	exit 1
fi

# curl reads the header from stdin so the key never appears in argv.
tb_get() {
	printf 'X-Authorization: ApiKey %s\n' "$TB_API_KEY" |
		curl -fsS -H @- "${TB_URL}${1}"
}

urlencode() {
	jq -rn --arg v "$1" '$v|@uri'
}

require_jq() {
	if ! command -v jq >/dev/null 2>&1; then
		echo "jq is required (brew install jq)" >&2
		exit 1
	fi
}

# Two lookups, because which one is allowed depends on the key's user. A
# tenant administrator can ask for a device by name directly; a customer user
# gets a 403 for that and has to page its own device list instead. Reading
# telemetry needs neither -- only the device id -- so a read-only customer user
# is the right credential here and the fallback is the normal path, not a
# degraded one.
device_id() {
	local id
	id="$(tb_get "/api/tenant/devices?deviceName=$(urlencode "$TB_DEVICE_NAME")" 2>/dev/null |
		jq -r '.id.id // empty')" || true
	if [ -n "$id" ]; then
		echo "$id"
		return
	fi

	local customer
	customer="$(tb_get "/api/auth/user" | jq -r '.customerId.id // empty')"
	[ -n "$customer" ] || return 0
	tb_get "/api/customer/${customer}/devices?pageSize=200&page=0" |
		jq -r --arg name "$TB_DEVICE_NAME" '
			.data[]? | select((.name | ascii_downcase) == ($name | ascii_downcase))
			| .id.id' | head -1
}

require_jq
CMD="${1:-}"
[ -n "$CMD" ] || usage

ID="$(device_id)"
if [ -z "$ID" ]; then
	echo "No device named '${TB_DEVICE_NAME}' on ${TB_URL}" >&2
	exit 1
fi

# ThingsBoard timestamps are milliseconds since the epoch.
now_ms() { echo $(( $(date +%s) * 1000 )); }

fetch_series() {
	local hours="${1:-24}" agg_s="${2:-}"
	local end start interval agg
	end="$(now_ms)"
	start=$(( end - hours * 3600 * 1000 ))
	if [ -n "$agg_s" ]; then
		interval=$(( agg_s * 1000 ))
		agg="AVG"
	else
		interval=0
		agg="NONE"
	fi
	# limit is per key. 50000 is ThingsBoard's own ceiling for this endpoint;
	# at one sample every five seconds a day is ~17k, so a raw week needs
	# aggregation rather than a bigger limit.
	tb_get "/api/plugins/telemetry/DEVICE/${ID}/values/timeseries?keys=${KEYS}&startTs=${start}&endTs=${end}&interval=${interval}&limit=50000&agg=${agg}&orderBy=ASC"
}

case "$CMD" in
latest)
	tb_get "/api/plugins/telemetry/DEVICE/${ID}/values/timeseries?keys=${KEYS}" |
		jq -r 'to_entries
			| sort_by(.key)[]
			| "\(.key)\t\(.value[0].value)\t\((.value[0].ts/1000) | strflocaltime("%Y-%m-%d %H:%M:%S"))"' |
		column -t -s $'\t'
	;;
series)
	fetch_series "${2:-24}" "${3:-}"
	;;
csv)
	# One row per timestamp, columns in a fixed order, so the output can go
	# straight into anything that reads CSV.
	fetch_series "${2:-24}" "${3:-}" | jq -r --arg keys "$KEYS" '
		($keys | split(",")) as $k
		| (reduce (to_entries[]) as $e ({};
			reduce ($e.value[]) as $p (.;
				.[$p.ts | tostring] += { ($e.key): $p.value })))          as $rows
		| (["ts","time"] + $k), (
			$rows | to_entries | sort_by(.key | tonumber)[]
			| [ .key, ((.key | tonumber / 1000) | strflocaltime("%Y-%m-%d %H:%M:%S")) ]
			  + [ $k[] as $n | (.value[$n] // "") ]
		  )
		| @csv'
	;;
attrs)
	tb_get "/api/plugins/telemetry/DEVICE/${ID}/values/attributes" |
		jq -r 'sort_by(.key)[]
			| "\(.key)\t\(.value)\t\((.lastUpdateTs/1000) | strflocaltime("%Y-%m-%d %H:%M"))"' |
		column -t -s $'\t'
	;;
keys)
	tb_get "/api/plugins/telemetry/DEVICE/${ID}/keys/timeseries" | jq -r '.[]'
	;;
*)
	usage
	;;
esac
