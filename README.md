# Curing Chamber Fanbox

Firmware for an ESP32-S3 that ventilates a curing chamber -- a fridge converted for
dry-aging meat -- and reports to a [ThingsBoard](https://thingsboard.io/) instance over
MQTT. Air enters through a vent at the bottom of the fridge and leaves through the fan at
the top; temperature is held by an external Inkbird controller switching the compressor.

**Ventilation is a timer, not a humidity controller.** The fan's job is fresh air: a
couple of bounded bursts a day, at wall-clock times. Humidity can *ask for more* air when
the chamber is drying out faster than intended, but it can never ask for less, because
the fan has no way to dry the chamber -- only the evaporator plate does that.

That distinction is the whole point of this rework. The previous firmware ran the fan as
a bang-bang humidity controller, holding it on for hours at a time. Room air at 20-24 °C
carries far more water than chamber air at 10 °C, and all of it ended up on the coldest
surface in the box: the evaporator plate froze over, which stops it dehumidifying, which
makes the controller run the fan harder. The meat itself gives off an order of magnitude
more moisture than a scheduled burst carries in, so a chamber with meat in it does not
need a fan to stay humid.

The safeguards that follow from that:

- Every burst has an end. Nothing in the state machine can hold the fan on indefinitely.
- An optional probe on the evaporator plate lets a cold plate **defer** a burst until it
  warms and drains, up to a bounded timeout.
- Extra humidity-driven bursts are capped per day.
- A daily minimum of fan time is spread across the day, so a chamber that never asks for
  air still gets some.

Fan speed is set by a panel potentiometer; ventilation decides only *when* the fan runs,
and enforces a minimum duty while it does so that a knob turned to zero cannot silently
disable it. Schedule, thresholds and the manual override are all adjustable remotely as
ThingsBoard shared attributes.

Commit `57b09f3` is the as-built reference snapshot: the firmware that ran the chamber
before any of this rework, kept as a baseline. Everything since builds against
ESP-IDF 6.0.

## Hardware

| | |
|---|---|
| MCU | ESP32-S3 (ESP32-S3-Nano form factor), 16 MB flash, 8 MB PSRAM |
| Fan controller | EMC2101 — PWM output + tachometer, I²C `0x4C` |
| Climate sensor | SHT31 — temperature + relative humidity, I²C `0x44` |
| Speed control | Potentiometer on ADC1 channel 0 |
| Fan power | Switched on `D2` |
| Plate probe | DS18B20 on 1-Wire, optional — see [Plate probe](#plate-probe) |

Board photos are in `ESP32-S3-Nano_Version3.jpg` and `ESP32-S3-Nano_Version4.jpg`.

### Pinout

| Signal | Pin | GPIO |
|---|---|---|
| I²C SDA | `A4` | 11 |
| I²C SCL | `A5` | 12 |
| Potentiometer | `A0` | 1 |
| Fan enable | `D2` | 5 |
| Plate probe 1-Wire | `D3` | 6 |

Full board mapping is in [`main/pins.h`](main/pins.h).

## Architecture

FreeRTOS tasks, communicating through mutex-guarded `AtomicVariable` values that
optionally persist to NVS:

| Task | Period | Role |
|---|---|---|
| `VentilationTask` | 1 s | Runs the ventilation state machine, switches the fan, detects a stall |
| `ClimateSensorTask` | 1 s | Reads temperature and humidity from the SHT31 |
| `PlateProbeTask` | 5 s | Reads the evaporator plate temperature, if a probe is fitted |
| `PotentiometerTask` | 250 ms | Samples the knob, maps it to a 0–255 duty cycle |
| `FanTask` | 100 ms | Applies duty cycle to the EMC2101, reads back tachometer RPM |
| `TelemetryTask` | 5 s | Publishes telemetry |
| `PublishAttributesTask` | on change | Publishes attributes |

The decision logic itself is deliberately separate from the task that runs it.
[`main/control/VentilationPolicy`](main/control/VentilationPolicy.h) is a pure state
machine with no ESP-IDF dependency: it takes a timestamp, a humidity reading, a plate
reading and the settings, and returns whether the fan should be on. That is what makes it
testable on the host — see [Tests](#tests) — which matters more here than usual, since the
failure it exists to prevent takes a day to reproduce on hardware.

### Ventilation

States: `idle` → `pending` → `running` → `settling`.

A slot comes due every `vent_interval_hours`, aligned to `vent_first_hour_local` when the
clock is synced and to time since boot when it is not. The burst then has to get past the
plate gate: if a probe is fitted and reads below `plate_gate_temp_c` — or has gone stale,
which is treated as cold — the burst waits in `pending` until the plate warms, or until
`vent_max_defer_minutes` runs out and it goes ahead anyway. After a burst the machine
sits in `settling` for `vent_settle_minutes`, long enough for the chamber to mix and the
SHT31 to catch up, before any new trigger is considered.

Four things can trigger a burst:

| Trigger | Condition |
|---|---|
| scheduled | the interval elapsed |
| dry | humidity below `humidity_setpoint − humidity_undershoot_limit`, capped at `vent_max_dry_bursts_per_day` |
| make-up | the day's fan time is behind the prorated share of `vent_min_seconds_per_day` |
| manual | `vent_now` set in ThingsBoard — bypasses the plate gate |

`vent_now` is edge-triggered: ThingsBoard holds a shared attribute until something clears
it, so a level trigger would ventilate forever. Only the rising edge counts, and the
attribute is not persisted to NVS.

The daily counters follow the local calendar day when the clock is synced. A boot in the
middle of a day credits the part of the day already elapsed, so a device that reboots at
18:00 does not immediately fire a day's worth of make-up bursts.

Setting `ctrl_loop_enabled` false hands the fan to `fan_enabled` directly. The policy is
left running rather than reset, so switching automatic back on resumes the schedule
instead of restarting it.

### Plate probe

The evaporator plate is the only dehumidifier in the chamber and the surface frost forms
on, so its temperature is what tells the firmware whether a burst is safe. It is optional:
with no probe fitted the schedule still runs, just ungated.

Configure it under `idf.py menuconfig` → *Curing chamber fanbox*, or in
[`sdkconfig.defaults`](sdkconfig.defaults):

```
CONFIG_PLATE_PROBE_DS18B20=y
CONFIG_PLATE_PROBE_GPIO=6
```

Wiring is three conductors: data to the configured GPIO, plus 3V3 and ground. **A 4.7 kΩ
pull-up from the data line to 3V3 is required** — the driver does not enable an internal
one and the bus does not work reliably without it. Do not use GPIO 5 (`D2`), which
switches the fan, or GPIO 33-37, which carry the octal PSRAM.

A probe that appears after boot is picked up on a later sweep, so the firmware can be
flashed before the probe is wired. A reading older than 30 s is reported as stale, which
the policy treats the same as a cold plate.

### Clock

SNTP is started once the network is up (`CONFIG_NTP_SERVER`, default `pool.ntp.org`), and
the local time zone comes from `CONFIG_TIMEZONE` — a POSIX `TZ` string with DST rules,
defaulting to Central European Time. Nothing blocks on the sync: until it lands,
ventilation schedules on time since boot instead of wall-clock hours, and the daily
counters use rolling 24-hour windows instead of calendar days.

Console logs are additionally intercepted (`esp_log_set_vprintf`), batched, and shipped
to ThingsBoard as telemetry — see [`main/log_streamer.cpp`](main/log_streamer.cpp).

Supporting pieces: a dependency-free JSON builder and parser
([`main/JsonBuilder`](main/JsonBuilder), [`main/JsonParser`](main/JsonParser)) and a small
Arduino-flavoured I²C abstraction ([`main/I2C`](main/I2C)).

## MQTT interface

**Telemetry** → `v1/devices/me/telemetry`

`fan_duty_cycle`, `fan_rpm`, `temperature`, `humidity`, `plate_temperature`, plus batched
`log` objects. `plate_temperature` is published as JSON `null` when a probe is configured
but its reading is stale, so a gap in the chart is a gap and not a plausible-looking
frozen value.

**Attributes** → `v1/devices/me/attributes`

`fan_running`, `fan_enabled`, `vent_state`, `vent_next_seconds`, `plate_probe`,
`fan_stalled`, `ram_free`, `ram_total`.

`vent_next_seconds` is `-1` when nothing is scheduled. `fan_stalled` goes true when the
fan has been commanded on for five seconds and the tachometer still reads zero — most
often the speed knob turned to zero, which otherwise looks exactly like a working
ventilation burst from every other reading on the dashboard.

**Shared attributes** (set from ThingsBoard, persisted to NVS on the device). Telemetry
is *not* persisted — see [Flash wear](#flash-wear):

| Key | Default | Meaning |
|---|---|---|
| `ctrl_loop_enabled` | `true` | Automatic ventilation; when false, `fan_enabled` is obeyed directly |
| `fan_enabled` | `true` | Manual fan state, used when automatic ventilation is off |
| `vent_now` | `false` | Rising edge ventilates immediately, ignoring the plate gate. Not persisted |
| `vent_interval_hours` | `12.0` | How often a scheduled burst comes due |
| `vent_first_hour_local` | `6` | Local hour the day's first slot is anchored to |
| `vent_burst_seconds` | `90.0` | Length of one burst |
| `vent_settle_minutes` | `20.0` | Quiet period after a burst before any new trigger |
| `vent_min_seconds_per_day` | `180.0` | Daily floor of fan time, spread across the day |
| `vent_max_dry_bursts_per_day` | `6` | Cap on extra humidity-driven bursts |
| `vent_max_defer_minutes` | `360.0` | How long a cold plate may hold a burst back |
| `vent_min_duty_percent` | `30` | Minimum fan duty while a burst runs, regardless of the knob |
| `plate_gate_temp_c` | `2.0` | Plate temperature a burst needs to see before it runs |
| `humidity_setpoint` | `75.0` | Target relative humidity, % |
| `humidity_undershoot_limit` | `5.0` | Humidity asks for an extra burst below setpoint − this |
| `uart_log_level` | `DEBUG` | Console log level |
| `streamer_log_level` | `INFO` | Level threshold for logs shipped over MQTT |

There is no overshoot limit any more. It described the fan switching *off* above a
humidity threshold, and the fan cannot lower humidity, so it never meant anything.

## Building

Requires ESP-IDF **6.0** and an ESP32-S3 target. `esp-mqtt` is pulled in by the IDF
Component Manager from [`main/idf_component.yml`](main/idf_component.yml).

```bash
idf.py set-target esp32s3
idf.py build
idf.py -p /dev/cu.usbmodemXXXX flash monitor
```

`sdkconfig` is generated from [`sdkconfig.defaults`](sdkconfig.defaults) and is not
committed.

### Credentials

No credential is compiled into the firmware. WiFi and ThingsBoard details are read at
boot from the NVS namespace `provision`, written once per device:

```bash
cp provisioning/secrets.csv.example provisioning/secrets.csv
$EDITOR provisioning/secrets.csv
./provisioning/provision.sh /dev/cu.usbmodemXXXX
```

An unprovisioned device stops at boot with a repeating error rather than falling back to
a built-in default. See [`provisioning/README.md`](provisioning/README.md).

### Flash layout

[`partitions.csv`](partitions.csv) maps the full 16 MB: two 3 MB OTA slots, then
`otadata`, `phy_init` and a 256 KB NVS above them. `0x9000-0xFFFF` is left unmapped —
that is where the original NVS partition sat, and it is past its rated erase endurance.

### Releasing an update

`PROJECT_VER` in the top-level [`CMakeLists.txt`](CMakeLists.txt) is baked into the app
descriptor and reported to ThingsBoard as `current_fw_version`. Bump it, tag the commit
`vX.Y.Z`, and push the tag. CI refuses to build a tag that disagrees with `PROJECT_VER` —
a package whose version differs from what the image reports installs and then still looks
out of date, which is an update loop.

The tagged build attaches the image and its SHA-256 to a GitHub release:

```
https://github.com/larssonwassen/curing-chamber-fanbox/releases/download/vX.Y.Z/curing-chamber-fanbox-X.Y.Z.bin
```

Every build, tagged or not, also uploads the image as a workflow artifact, which is the
convenient way to test a branch without building locally.

Note that the device implements ThingsBoard's MQTT chunk protocol only. A package created
as an *external URL* is not enough — the binary has to be stored in ThingsBoard itself,
which is why the release asset is a convenience for humans and not something the device
can be pointed at.

#### Publishing to ThingsBoard

The same tagged build uploads the image to ThingsBoard as a firmware package, using
[`tools/tb_upload_firmware.sh`](tools/tb_upload_firmware.sh). It creates the package,
uploads the binary with the SHA-256 CI computed, and stops there — it does **not** assign
the package to anything. Assignment is what actually starts an update on the device in the
chamber, so it stays a manual click made by someone who can watch what happens.

The step is skipped when the settings below are absent, so the build still works on a fork
or in a repo that has no ThingsBoard.

| Setting | Kind | Value |
| --- | --- | --- |
| `TB_URL` | variable | e.g. `https://thingsboard.example.com` |
| `TB_API_KEY` | secret | ThingsBoard API key, created under the user's profile |
| `TB_DEVICE_PROFILE_NAME` | variable | device profile the package belongs to, e.g. `default` |

Repository settings → Secrets and variables → Actions; variables and secrets are separate
tabs there. `TB_DEVICE_PROFILE_ID` may be given as a variable instead of the name, which
skips the lookup.

An API key authenticates as `X-Authorization: ApiKey <key>` and carries the permissions of
the user that created it; creating an OTA package needs tenant-administrator rights. A key
is preferable to a password in CI because it can be disabled on its own, without changing
a login anyone still uses — worth doing the moment a build looks wrong, since the secret
sits in a repository that anyone with push access can run workflows in.

`TB_USERNAME` and `TB_PASSWORD` still work as a fallback, logging in for a JWT, for an
instance where API keys are unavailable.

The script also runs from a laptop, against a locally built image:

```bash
TB_URL=https://thingsboard.example.com TB_API_KEY=tb_... \
TB_DEVICE_PROFILE_NAME=default \
./tools/tb_upload_firmware.sh build/curing-chamber-fanbox.bin 0.4.0
```

Re-running it for a version that already exists is a no-op — ThingsBoard rejects a
duplicate title and version, so the script checks first and exits cleanly.

### Tests

The parsing code and the ventilation state machine are target-independent and have host
tests under ASan/UBSan:

```bash
make -C test/host
```

[`test/host/test_ventilation_policy.cpp`](test/host/test_ventilation_policy.cpp) covers
the bounds that keep the plate clear: bursts end, a cold plate defers and a stale probe
does not defer forever, humidity only ever asks for more air, the daily counters reset on
a day boundary, and scheduling survives the 32-bit millisecond wrap that arrives every
49 days.

## Updates

Firmware is pulled over ThingsBoard's MQTT OTA protocol — see
[`main/ota/OtaUpdater.cpp`](main/ota/OtaUpdater.cpp). ThingsBoard announces the target as
shared attributes (`fw_title`, `fw_version`, `fw_size`, `fw_checksum`,
`fw_checksum_algorithm`); the device pulls the image a chunk at a time and reports
progress back as an `fw_state` telemetry value.

Fragments are written straight into the inactive OTA slot as they arrive, so nothing
buffers a whole chunk. The image is only booted once its digest matches the announced
`fw_checksum`; SHA-256/384/512 are accepted and everything else is refused rather than
installed unverified.

The bootloader boots a new image in the pending-verify state. It is confirmed only after
WiFi *and* the broker are both reachable on the new firmware — an image that cannot get
that far rolls itself back on the next reset instead of stranding a device inside a
sealed chamber.

Rollback needs a reset to happen, so an unconfirmed image that stays up and simply cannot
reach the broker would otherwise sit there forever. A ten-minute timer covers that: an
image still unconfirmed when it expires reboots itself, and the bootloader then takes the
previous slot. Only images in pending-verify are on that timer — a confirmed image waits
for the broker indefinitely, as it should.

## Flash wear

The unit that ran the chamber persisted every telemetry update to NVS. `fanRPM` changes
about ten times a second, and each change committed to flash; by the time it was noticed
the active page sequence number was 1,017,540 — roughly 200,000 erase cycles per sector
against a rated ~100,000.

Telemetry no longer carries NVS keys, the stale keys are purged once at boot, and
`nvs_get_stats()` is logged at startup so the partition's usage is visible rather than
silent. NVS has also moved off the worn sectors entirely.

## Known limitations

- **PSRAM mode is unverified.** `CONFIG_SPIRAM_MODE_OCT` was inferred from the module
  reporting 8 MB of embedded PSRAM (ESP32-S3R8). The wrong mode fails loudly at boot, so
  this needs one flash-and-check on hardware.
- **The ventilation rework has not run through a full drying session yet.** The state
  machine is covered by host tests and the plate gate behaves on the bench, but the thing
  it exists to prevent — a plate frosting over across days — is only ever confirmed by
  running one.
- **MQTT defaults to plaintext.** `mqtts://` is supported and verifies against the bundled
  root CAs, but which one is used depends on the provisioned `tb_uri`.

## License

[MIT](LICENSE)
