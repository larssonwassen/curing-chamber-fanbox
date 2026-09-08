# Curing Chamber Fanbox

Firmware for an ESP32-S3 that regulates humidity inside a curing chamber by driving a
PWM fan, and reports to a [ThingsBoard](https://thingsboard.io/) instance over MQTT.

A humidity setpoint with separate overshoot/undershoot limits gives the control loop
hysteresis, so the fan is not cycled continuously around the target. Fan speed itself is
set by a panel potentiometer read on the ADC; the control loop decides only whether the
fan runs. Both the setpoint and the manual override are adjustable remotely as
ThingsBoard shared attributes.

> **Status: reference snapshot.** This is the as-built firmware that has been running the
> chamber, published as a baseline before a planned refactor. It builds against
> ESP-IDF 5.3 and is known not to build on 6.x yet. See [Known limitations](#known-limitations).

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

**Shared attributes** (set from ThingsBoard, persisted to NVS on the device):

| Key | Default | Meaning |
|---|---|---|
| `ctrl_loop_enabled` | `true` | Automatic humidity control; when false, `fan_enabled` is obeyed directly |
| `humidity_setpoint` | `75.0` | Target relative humidity, % |
| `humidity_overshoot_limit` | `5.0` | Fan switches on above setpoint + this |
| `humidity_undershoot_limit` | `5.0` | Fan switches off below setpoint − this |
| `fan_enabled` | `true` | Manual fan state, used when the control loop is off |
| `uart_log_level` | `DEBUG` | Console log level |
| `streamer_log_level` | `INFO` | Level threshold for logs shipped over MQTT |

## Building

Requires ESP-IDF **5.3** and an ESP32-S3 target.

Credentials are placeholders in this snapshot and must be filled in before building:

- WiFi SSID and PSK — [`main/main.cpp`](main/main.cpp)
- ThingsBoard host, device ID, access token — [`main/mqtt.h`](main/mqtt.h)

```bash
idf.py set-target esp32s3
idf.py build
idf.py -p /dev/cu.usbmodemXXXX flash monitor
```

## Known limitations

Documented honestly, since this snapshot is a baseline rather than a finished product.

- **Credentials are compiled into the binary.** They belong in NVS, provisioned at flash
  time, so that firmware images can be shared without leaking them. MQTT is also plaintext
  (`mqtt://`) rather than TLS.
- **Telemetry values persist to NVS on every change.** `fanRPM` updates ten times a second,
  each triggering an `nvs_commit`. On the unit that has been running the chamber, the NVS
  pages have cycled over a million times — roughly twice the flash's rated erase endurance.
  Telemetry should not be persisted at all.
- **No OTA.** The partition table declares 2 MB of a 16 MB flash with a single 1 MB app
  slot, leaving no room for A/B updates and no rollback if an update fails.
- **8 MB of PSRAM is unconfigured** and therefore entirely unused.
- **Does not build on ESP-IDF 6.x.** Needs a config regeneration and API migration.
- Several buffer-handling paths in the log streamer and JSON parser need bounds fixes.
- `main/CMakeLists.txt` refers to `JSONBuilder/` where the directory is `JsonBuilder/`;
  this only builds on case-insensitive filesystems such as macOS APFS.

## License

[MIT](LICENSE)
