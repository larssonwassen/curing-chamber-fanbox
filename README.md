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
- A burst is not a commitment. Its reasons are re-read on every tick, so a plate that goes
  cold or a chamber that stops being dry ends the burst early.
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
| Plate probe 1-Wire | `D13` | 48 |

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
which is treated as cold — the burst waits in `pending` until the plate warms.

What happens if it never warms depends on *why* the burst was asked for. Air exchange has
to happen eventually, so a scheduled or make-up burst goes ahead anyway once
`vent_max_defer_minutes` runs out. A humidity burst never does: forcing moist air onto
sub-zero metal is the mechanism that armoured the evaporator in the first place, and
nothing goes wrong if it waits, because the fan cannot dry the chamber — a humidity burst
that never runs is one that was not needed.

**Nothing latches.** The policy keeps history — when the last burst ran, which slot has
been served, how much fan time the day has had — but not intent. A burst that is owed and
a burst that is running both re-read the plate and the humidity every second, and stop as
soon as their reasons stop holding. This is not a refinement; it is the difference between
the gate working and the gate being decorative. Two failures on the real chamber came from
the earlier latching version:

- A burst started the instant the plate touched the gate on its way up, the compressor cut
  back in, and the fan kept running for another 85 seconds while the plate dived to
  −2.6 °C — pushing moist room air onto sub-zero metal, which is the exact event the gate
  exists to prevent.
- A humidity burst raised at 63% RH waited seven minutes for the plate and then fired into
  a chamber that had climbed to 72.7%, above its own setpoint. Humidity rises steeply
  exactly while a burst is pending, because the same warming plate that opens the gate is
  giving its frost back to the air.

Re-reading conditions continuously invites the opposite failure — a condition sitting on
its threshold cycling the fan at the loop rate — so three things bound it. A humidity burst
is *raised* at `humidity_setpoint - humidity_undershoot_limit` but *held* until
`humidity_setpoint`, which is a deadband made of numbers that were already configured. The
plate must fall 0.3 °C below `plate_gate_temp_c` before it cuts a running burst, which is
three counts of the DS18B20's resolution and so cannot be triggered by quantisation noise.
And no burst may be stopped inside its first five seconds, whatever the reason. A burst
that overrode the gate to start — a manual request, or a scheduled one deferred past
`vent_max_defer_minutes` — is not subject to the gate afterwards, since cutting it off on
the first tick would amount to never having forced it through.

There is no daily cap on humidity-driven bursts. Burst-then-settle is the bound, and how
tight a bound depends on `vent_settle_minutes`: at the 1.5-minute default a chamber with
humidity pinned at the floor cycles at about 50% duty, while a longer settle trades
responsiveness for a lower ceiling. Either way every burst ends, and the plate gate stops
the cycle outright once the evaporator goes cold — which is the protection that matters,
the duty ceiling being a second line rather than the first. After a burst the machine
sits in `settling` for `vent_settle_minutes`, long enough for the chamber to mix and the
SHT31 to catch up, before any new trigger is considered.

Four things can trigger a burst:

| Trigger | Condition |
|---|---|
| scheduled | the interval elapsed |
| dry | *average* humidity below `humidity_setpoint − humidity_undershoot_limit` |
| make-up | the day's fan time is behind the prorated share of `vent_min_seconds_per_day` |
| manual | `vent_now` set in ThingsBoard — bypasses the plate gate |

The dry trigger reads an average, not the sensor. This chamber's own compressor swings
relative humidity by tens of points every half hour: measured over one clean 47-minute
cycle with the fan idle, the air went from 3.38 to 6.60 g water per kg — half its own peak
— as frost formed on the sub-zero plate and came back off it, and RH swung 27 points with
it. An instantaneous reading therefore reports compressor phase at least as much as it
reports how much water the chamber holds, and a trigger raised on it fires at the trough of
every cycle regardless of the chamber's actual state. `humidity_average_minutes` sets the
time constant of a first-order filter on that reading; at the 30-minute default, against a
31–47 minute cycle, a ±13.5 point swing arrives at the trigger as ±2.5. Set it to zero to
read the sensor directly. The filtered value is published as `humidity_avg` beside the raw
`humidity`, because the gap between the two traces is the thing worth seeing. The trigger
stays silent for one full time constant after boot, since until then the average is still
mostly the single reading it was seeded from.

Only the raise reads the average; a burst already running stops on the raw reading. The two
questions are different. "Is this chamber dry?" is about the chamber's state, and the swing
has to come out of it first. "Has this burst delivered enough air yet?" is about what the
fan just did, and the filter is deliberately far too slow to see that.

Whatever asked for it, **a burst serves the schedule slot it ends in**. Fresh air is fresh
air, so a humidity burst at 07:00 satisfies the 06:00 slot and no scheduled burst follows.
Without this the schedule cannot tell that the chamber has just been ventilated: on
2026-09-09 a humidity burst finished ten seconds into the 18:00 slot, the scheduled burst
ran ninety seconds later, and between them they took the chamber from 63% to 87% RH.
`vent_min_seconds_per_day` remains the floor on total air, so displacing scheduled bursts
this way cannot starve the chamber. The no-clock path always behaved this way, because it
measures the interval from the end of the last burst rather than from a slot boundary.

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
CONFIG_PLATE_PROBE_GPIO=48
```

Wiring is three conductors: data to the configured GPIO, plus 3V3 and ground. The
internal pull-up is enabled, so no external resistor is needed over a short lead — but it
is roughly 45 kΩ where 1-Wire wants 4.7 kΩ, and a long probe cable adds capacitance that
rounds off the rising edge until a one is sampled as a zero. That shows up as intermittent
bad reads rather than a dead bus, so **fit a 4.7 kΩ resistor from data to 3V3 if the probe
reads erratically** before suspecting the probe itself.

GPIO 48 is also `SCK`, which costs nothing here because this firmware uses no SPI. Do not
use GPIO 5 (`D2`), which switches the fan, or GPIO 33-37, which carry the octal PSRAM.

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

`fan_duty_cycle`, `fan_rpm`, `temperature`, `humidity`, `plate_temperature`,
`vent_state` (0 idle, 1 pending, 2 running, 3 settling) and `vent_gate_blocked`, plus
batched `log` objects. These are published every five seconds.

`vent_state` is sampled rather than reported on change, which it used to be, because a
chart cannot draw a state that only appears when it moves. Between two change events there
are no points at all, so the line is drawn straight across the gap — and a settling-to-idle
ramp passes through the level that means pending on the way down, showing a state the
chamber was never in. Sampling also makes the average honest: over a day the mean of
`vent_gate_blocked` is the fraction of the day ventilation spent vetoed, whereas the mean
of a change event counts transitions rather than time.

The settings that shape the control loop are published as telemetry too, but **only when
they change**: `ctrl_loop_enabled` and every `vent_*` / `humidity_*` / `plate_gate_temp_c`
setting. A setpoint holds for days, so sampling one alongside the sensors would store the
same number a million times to describe an event that happened twice. One snapshot is also
published on every MQTT connect, so a chart has something to anchor to after a reboot
instead of waiting hours for the first change.

This duplicates values that are also attributes, deliberately. ThingsBoard keeps only the
current value of an attribute, which answers "what is the setpoint" and cannot answer
"what was the setpoint at 03:00 on Tuesday" — the question that matters when reading back
a week of chamber behaviour. `vent_state` is numeric here and a word in the attribute, for
the same split of purposes: charts cannot plot `settling`, and tiles should not show `3`.

`vent_gate_blocked` is worth calling out: it is 1 while a burst is owed and the plate is
too cold to take it. Nothing else records that. `fan_rpm` is zero whether ventilation is
idle or being vetoed, so without this key the one thing the plate probe was fitted to
measure — how much of the day the gate is holding air back — is invisible. `plate_temperature` is published as JSON `null` when a probe is configured
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
| `ota_on_dev_build` | `false` | Let an OTA package install over a hand-flashed build |
| `fan_enabled` | `true` | Manual fan state, used when automatic ventilation is off |
| `vent_now` | `false` | Rising edge ventilates immediately, ignoring the plate gate. Not persisted |
| `vent_interval_hours` | `12.0` | How often a scheduled burst comes due |
| `vent_first_hour_local` | `6` | Local hour the day's first slot is anchored to |
| `vent_burst_seconds` | `90.0` | Length of one burst |
| `vent_settle_minutes` | `1.5` | Quiet period after a burst before any new trigger |
| `vent_min_seconds_per_day` | `180.0` | Daily floor of fan time, spread across the day |
| `vent_max_defer_minutes` | `360.0` | How long a cold plate may hold a burst back |
| `vent_min_duty_percent` | `30` | Minimum fan duty while a burst runs, regardless of the knob |
| `plate_gate_temp_c` | `2.0` | Plate temperature a burst needs to see before it runs |
| `humidity_setpoint` | `75.0` | Target relative humidity, % |
| `humidity_undershoot_limit` | `5.0` | Humidity asks for an extra burst below setpoint − this |
| `humidity_average_minutes` | `30.0` | Time constant of the average the dry trigger reads; `0` reads the sensor |
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

#### Flashing over USB while an OTA package is assigned

A hand-flashed build declines OTA updates. Without that, bench-testing a change is a race
the cable loses: `idf.py flash` writes `ota_0`, the bootloader may still be pointed at
`ota_1`, and even after `idf.py erase-otadata` the device connects, sees an assigned
package whose version differs from the build just flashed, and re-installs it about forty
seconds later:

```
ota: Updating curing-chamber-fanbox 0.5.0-dirty -> 0.5.0 (1133840 bytes into ota_1)
```

The flash succeeds, the log looks right, and the firmware under test is gone before
anything can be observed.

The version string is what distinguishes the two: a release is exactly `0.5.1`, and a
build off a tag carries the rest of `git describe`, `0.5.1-2-gab12cd3-dirty`. Anything
that is not digits and dots is treated as a development build, `0.0.0-dev` included.

Set the `ota_on_dev_build` shared attribute to override it. That exists because declining
is a safe default rather than a safe answer: a hand-flashed build sealed inside a chamber
still has to be recoverable without opening it.

Flashing over a slot the bootloader is not pointed at is a separate trap, and
`erase-otadata` is the fix:

```bash
idf.py erase-otadata flash monitor
```

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

The version comes from the git tag. Tag a commit `vX.Y.Z` and push the tag — there is
nothing to edit first:

```bash
git tag v0.5.0 && git push origin v0.5.0
```

The tag string, minus the `v`, is baked into the app descriptor and reported to
ThingsBoard as `current_fw_version`. Because it *is* the tag, a package whose version
disagrees with what the image reports is not possible; that used to be a CI check, and
the failure it guarded against is an update loop — a device that installs a package and
still considers itself out of date.

A build off a tag reports something like `0.5.0-3-gab12cd3-dirty`, which is deliberate: a
hand-flashed build should not claim to be the release.

The tagged build attaches the image and its SHA-256 to a GitHub release:

```
https://github.com/larssonwassen/curing-chamber-fanbox/releases/download/vX.Y.Z/curing-chamber-fanbox-X.Y.Z.bin
```

Every build also uploads the image as a workflow artifact, which is the convenient way to
test a branch without building locally.

CI builds pull requests and tags, not pushes to `main`: a push to `main` is the merge of a
pull request that was already built, so building it again builds the same tree twice.
That relies on the repository requiring branches to be up to date before merging.

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

### Reading the chamber back

[`tools/tb_telemetry.sh`](tools/tb_telemetry.sh) pulls telemetry and attributes out of
ThingsBoard for analysis. It is read-only and writes nothing to ThingsBoard or the device.

```bash
./tools/tb_telemetry.sh latest          # newest value of every key
./tools/tb_telemetry.sh series 24       # last 24 h, every reading, as JSON
./tools/tb_telemetry.sh series 72 900   # last 72 h, 15-minute averages
./tools/tb_telemetry.sh csv 48 > chamber.csv
./tools/tb_telemetry.sh attrs           # includes vent_state and plate_probe
```

The credential is read from `~/.config/curing-chamber/tb_api_key` rather than an
environment variable or an argument, so it stays out of shell history and out of the
process table, and it is piped to `curl` as a header on stdin. Create it with:

```bash
mkdir -p ~/.config/curing-chamber && chmod 700 ~/.config/curing-chamber
printf '%s' 'the-key' > ~/.config/curing-chamber/tb_api_key
chmod 600 ~/.config/curing-chamber/tb_api_key
```

A ThingsBoard API key carries the permissions of the user that created it, so this wants a
different key from the one CI uploads firmware with: reading telemetry needs a customer
user, while creating an OTA package needs a tenant administrator. Two keys also means
either can be revoked without breaking the other. The device lookup handles both, because
a customer user is refused the tenant device-by-name endpoint and has to page its own
device list instead.

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
