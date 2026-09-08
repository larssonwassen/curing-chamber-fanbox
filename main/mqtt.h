#ifndef MQTT_CLIENT_H
#define MQTT_CLIENT_H

#include "mqtt_client.h"

// NOTE: Placeholder credentials. Replace with your own before building.
// These move to NVS-based provisioning (namespace "provision") in a follow-up,
// so that firmware binaries can be published without carrying secrets.
#define MQTT_URL "mqtt://your-thingsboard-host"
#define MQTT_DEIVCE_ID "YOUR_DEVICE_ID"
#define MQTT_ACCESS_TOKEN "YOUR_ACCESS_TOKEN"

extern esp_mqtt_client_handle_t mqtt_client;
void mqtt_setup(void);

#endif // MQTT_CLIENT_H