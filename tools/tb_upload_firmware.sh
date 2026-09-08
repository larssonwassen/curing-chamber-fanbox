#!/usr/bin/env bash
#
# Upload a firmware image to ThingsBoard as an OTA package.
#
#   TB_URL=https://tb.example.com \
#   TB_USERNAME=ci@example.com TB_PASSWORD=... \
#   TB_DEVICE_PROFILE_NAME=default \
#   ./tools/tb_upload_firmware.sh build/curing-chamber-fanbox.bin 0.4.0
#
# The package title is fixed to the project name, because that is what the
# device compares against: OtaUpdater ignores an announcement whose fw_title is
# not its own esp_app_desc_t project name.
#
# This uploads only. It deliberately does not assign the package to anything --
# assignment is what starts an update on a live device, and that should be a
# person's decision, made while looking at the device.
#
# Credentials come from the environment and are never echoed. Run with `set -x`
# at your peril.

set -euo pipefail

TITLE="curing-chamber-fanbox"

BIN="${1:-}"
VERSION="${2:-}"

if [ -z "$BIN" ] || [ -z "$VERSION" ]; then
	echo "usage: $0 <firmware.bin> <version>" >&2
	exit 2
fi
if [ ! -f "$BIN" ]; then
	echo "$BIN not found" >&2
	exit 1
fi

for var in TB_URL TB_USERNAME TB_PASSWORD; do
	if [ -z "${!var:-}" ]; then
		echo "$var is not set" >&2
		exit 2
	fi
done
if [ -z "${TB_DEVICE_PROFILE_ID:-}" ] && [ -z "${TB_DEVICE_PROFILE_NAME:-}" ]; then
	echo "Set TB_DEVICE_PROFILE_ID, or TB_DEVICE_PROFILE_NAME to look it up" >&2
	exit 2
fi

TB_URL="${TB_URL%/}"

api() {
	# api <method> <path> [curl args...]; prints the response body.
	local method="$1" path="$2"; shift 2
	curl -sS -f --max-time 300 -X "$method" "${TB_URL}${path}" \
		-H "X-Authorization: Bearer ${TOKEN}" "$@"
}

echo "Authenticating to ${TB_URL} as ${TB_USERNAME}"
# --data @- so the password never appears in the process list, where any other
# process on the machine could read it out of /proc.
if ! LOGIN_BODY="$(
	jq -nc --arg u "$TB_USERNAME" --arg p "$TB_PASSWORD" '{username:$u, password:$p}' |
	curl -sS -f --max-time 60 -X POST "${TB_URL}/api/auth/login" \
		-H 'Content-Type: application/json' --data @-
)"; then
	echo "Login to ${TB_URL} failed (401 above means the credentials are wrong)" >&2
	exit 1
fi
TOKEN="$(printf '%s' "$LOGIN_BODY" | jq -r '.token')"
if [ -z "$TOKEN" ] || [ "$TOKEN" = "null" ]; then
	echo "Login succeeded but returned no token" >&2
	exit 1
fi
unset LOGIN_BODY

PROFILE_ID="${TB_DEVICE_PROFILE_ID:-}"
if [ -z "$PROFILE_ID" ]; then
	echo "Resolving device profile '${TB_DEVICE_PROFILE_NAME}'"
	PROFILE_ID="$(
		api GET "/api/deviceProfileInfos?pageSize=200&page=0" |
		jq -r --arg n "$TB_DEVICE_PROFILE_NAME" \
			'.data[] | select(.name == $n) | .id.id' | head -n1
	)"
	if [ -z "$PROFILE_ID" ]; then
		echo "No device profile named '${TB_DEVICE_PROFILE_NAME}'" >&2
		exit 1
	fi
fi
echo "Device profile: ${PROFILE_ID}"

# ThingsBoard rejects a duplicate title+version outright. Check first so the
# failure reads as "already published" rather than a 400 from deep in the API.
EXISTING="$(
	api GET "/api/otaPackages?pageSize=200&page=0&textSearch=$(printf '%s' "$TITLE" | jq -sRr @uri)" |
	jq -r --arg t "$TITLE" --arg v "$VERSION" \
		'.data[] | select(.title == $t and .version == $v) | .id.id' | head -n1
)"
if [ -n "$EXISTING" ]; then
	echo "Package ${TITLE} ${VERSION} already exists (${EXISTING}); nothing to do."
	exit 0
fi

# sha256sum on Linux/CI, shasum on a stock macOS.
if command -v sha256sum > /dev/null; then
	CHECKSUM="$(sha256sum "$BIN" | cut -d' ' -f1)"
else
	CHECKSUM="$(shasum -a 256 "$BIN" | cut -d' ' -f1)"
fi
SIZE="$(wc -c < "$BIN" | tr -d ' ')"
echo "Creating package ${TITLE} ${VERSION} (${SIZE} bytes, sha256 ${CHECKSUM})"

PACKAGE_ID="$(
	jq -nc \
		--arg title "$TITLE" \
		--arg version "$VERSION" \
		--arg profile "$PROFILE_ID" \
		'{
			title: $title,
			version: $version,
			type: "FIRMWARE",
			deviceProfileId: { entityType: "DEVICE_PROFILE", id: $profile }
		}' |
	api POST "/api/otaPackage" -H 'Content-Type: application/json' --data @- |
	jq -r '.id.id'
)"
if [ -z "$PACKAGE_ID" ] || [ "$PACKAGE_ID" = "null" ]; then
	echo "Package creation returned no id" >&2
	exit 1
fi

# Hand ThingsBoard the checksum rather than letting it compute one: that turns
# the upload into a verified transfer instead of a hope. The device checks the
# same digest again when it installs.
echo "Uploading ${BIN}"
api POST "/api/otaPackage/${PACKAGE_ID}?checksumAlgorithm=SHA256&checksum=${CHECKSUM}" \
	-F "file=@${BIN}" > /dev/null

echo
echo "Uploaded ${TITLE} ${VERSION} as ${PACKAGE_ID}."
echo "It is NOT assigned to any device. Assign it in ThingsBoard when you want"
echo "the update to start."
