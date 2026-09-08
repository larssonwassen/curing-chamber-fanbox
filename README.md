# Curing Chamber Fanbox

Firmware for an ESP32-S3 that regulates humidity inside a curing chamber by driving a
PWM fan, and reports to a [ThingsBoard](https://thingsboard.io/) instance over MQTT.

**The fan raises humidity.** Running it pulls humid air into the chamber, so the control
loop switches it *on* when the chamber is too dry and *off* once the target is reached --
the opposite of a venting fan, and worth stating plainly because the thresholds read
backwards otherwise.

A humidity setpoint with separate overshoot/undershoot limits gives the loop hysteresis,
so the fan is not cycled continuously around the target. Fan speed itself is set by a
panel potentiometer read on the ADC; the control loop decides only whether the fan runs.
Both the setpoint and the manual override are adjustable remotely as ThingsBoard shared
attributes.

If the climate sensor stops responding, the loop holds the fan off rather than
regulating on a reading that has stopped updating. Humidifying blind is the direction
that grows mould; drying out is slower and visible.

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

Board photos are in `ESP32-S3-Nano_Version3.jpg` and `ESP32-S3-Nano_Version4.jpg`.

### Pinout

| Signal | Pin | GPIO |
|---|---|---|
| I²C SDA | `A4` | 11 |
| I²C SCL | `A5` | 12 |
| Potentiometer | `A0` | 1 |
| Fan enable | `D2` | 5 |

Full board mapping is in [`main/pins.h`](main/pins.h).

## Architecture

Six FreeRTOS tasks, communicating through mutex-guarded `AtomicVariable` values that
optionally persist to NVS:

| Task | Period | Role |
|---|---|---|
| `ControlLoopTask` | 5 s | Compares humidity against the setpoint band, switches the fan |
| `ClimateSensorTask` | 1 s | Reads temperature and humidity from the SHT31 |
| `ADCTask` | continuous | Samples the potentiometer, maps it to a 0–255 duty cycle |
| `FanTask` | 100 ms | Applies duty cycle to the EMC2101, reads back tachometer RPM |
| `TelemetryTask` | 5 s | Publishes telemetry |
| `PublishAttributesTask` | on change | Publishes attributes |

Console logs are additionally intercepted (`esp_log_set_vprintf`), batched, and shipped
to ThingsBoard as telemetry — see [`main/log_streamer.cpp`](main/log_streamer.cpp).

Supporting pieces: a dependency-free JSON builder and parser
([`main/JsonBuilder`](main/JsonBuilder), [`main/JsonParser`](main/JsonParser)) and a small
Arduino-flavoured I²C abstraction ([`main/I2C`](main/I2C)).

## MQTT interface

**Telemetry** → `v1/devices/me/telemetry`

`fan_duty_cycle`, `fan_rpm`, `temperature`, `humidity`, plus batched `log` objects.

**Attributes** → `v1/devices/me/attributes`

`fan_running`, `fan_enabled`, `ram_free`, `ram_total`.

**Shared attributes** (set from ThingsBoard, persisted to NVS on the device). Telemetry
is *not* persisted — see [Flash wear](#flash-wear):

| Key | Default | Meaning |
|---|---|---|
| `ctrl_loop_enabled` | `true` | Automatic humidity control; when false, `fan_enabled` is obeyed directly |
| `humidity_setpoint` | `75.0` | Target relative humidity, % |
| `humidity_overshoot_limit` | `5.0` | Fan switches **off** at setpoint + this |
| `humidity_undershoot_limit` | `5.0` | Fan switches **on** at setpoint − this |
| `fan_enabled` | `true` | Manual fan state, used when the control loop is off |
| `uart_log_level` | `DEBUG` | Console log level |
| `streamer_log_level` | `INFO` | Level threshold for logs shipped over MQTT |

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
| `TB_USERNAME` | secret | a ThingsBoard tenant-administrator login |
| `TB_PASSWORD` | secret | its password |
| `TB_DEVICE_PROFILE_NAME` | variable | device profile the package belongs to, e.g. `default` |

Repository settings → Secrets and variables → Actions; variables and secrets are separate
tabs there. `TB_DEVICE_PROFILE_ID` may be given as a variable instead of the name, which
skips the lookup. Creating an OTA package requires tenant-administrator rights in
ThingsBoard, so make a dedicated CI user rather than reusing a personal login: the
credential lives in a repository secret, and anyone who can push a workflow to the repo
can use it.

The script also runs from a laptop, against a locally built image:

```bash
TB_URL=https://thingsboard.example.com \
TB_USERNAME=ci@example.com TB_PASSWORD=... \
TB_DEVICE_PROFILE_NAME=default \
./tools/tb_upload_firmware.sh build/curing-chamber-fanbox.bin 0.4.0
```

Re-running it for a version that already exists is a no-op — ThingsBoard rejects a
duplicate title and version, so the script checks first and exits cleanly.

### Tests

The parsing code is target-independent and has host tests under ASan/UBSan:

```bash
make -C test/host
```

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
- **MQTT defaults to plaintext.** `mqtts://` is supported and verifies against the bundled
  root CAs, but which one is used depends on the provisioned `tb_uri`.

## License

[MIT](LICENSE)
