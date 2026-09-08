#ifndef MQTT_CLIENT_H
#define MQTT_CLIENT_H

#include "mqtt_client.h"

// The broker URI and access token are provisioned into NVS per device; see
// DeviceConfig and provisioning/README.md. Nothing credential-bearing is
// compiled in, so the same binary runs on every device.

extern esp_mqtt_client_handle_t mqtt_client;
void mqtt_setup(void);

#endif // MQTT_CLIENT_H