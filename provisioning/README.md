# Provisioning

Credentials are not compiled into the firmware. They live in the NVS namespace
`provision`, written once per device, so a firmware binary — an OTA image, a
release artifact — carries no secrets and is identical for every device.

The firmware reads four keys at boot and refuses to start without them:

| Key         | Meaning                                                      |
| ----------- | ------------------------------------------------------------ |
| `wifi_ssid` | WiFi network name                                             |
| `wifi_psk`  | WiFi passphrase                                               |
| `tb_uri`    | Broker URI, e.g. `mqtt://host` or `mqtts://host:8883`         |
| `tb_token`  | ThingsBoard device access token, sent as the MQTT username    |

There is deliberately no compile-time fallback. An unprovisioned device stops
with a repeating error rather than quietly joining whatever network a default
happened to name.

## Writing them

```bash
cp provisioning/secrets.csv.example provisioning/secrets.csv
$EDITOR provisioning/secrets.csv
source ~/.espressif/tools/activate_idf_v6.0.2.sh
./provisioning/provision.sh /dev/cu.usbmodem3101
```

`secrets.csv` and the generated `nvs.bin` are gitignored.

The script writes only the `nvs` partition, reading its offset and size from
`partitions.csv`. The application is left alone, so re-provisioning a deployed
device is not a reflash.

## TLS

`tb_uri`'s scheme decides the transport. `mqtts://` attaches ESP-IDF's bundled
root CA set and verifies the broker's certificate; `mqtt://` is plaintext. That
choice is provisioned rather than built in, so moving a device onto TLS is a
re-provision.

## Rotating

Re-run `provision.sh` with the new values. Nothing else has to change.
