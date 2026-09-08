#ifndef _DEVICE_CONFIG_H
#define _DEVICE_CONFIG_H

#include "esp_err.h"

/**
 * @brief Credentials and endpoints read from NVS at boot.
 *
 * Nothing here is compiled into the firmware. The values live in the NVS
 * namespace "provision", written once per device at flash time (see
 * provisioning/README.md), so a firmware binary -- an OTA image, a GitHub
 * release artifact -- carries no credentials and is the same for every device.
 *
 * There is deliberately no compile-time fallback. A device that has not been
 * provisioned must fail loudly rather than quietly join whatever network a
 * default happened to name.
 */
class DeviceConfig {
public:
	/// NVS namespace holding the provisioned values.
	static constexpr const char* NVS_NAMESPACE = "provision";

	/**
	 * @brief Read every provisioned value from NVS.
	 *
	 * @return ESP_OK when all required keys were present and non-empty,
	 *         ESP_ERR_NVS_NOT_FOUND when the namespace or a key is missing,
	 *         or the underlying NVS error otherwise. Every missing key is
	 *         logged by name.
	 */
	static esp_err_t load(void);

	/// True once load() has returned ESP_OK.
	static bool isLoaded(void);

	static const char* wifiSsid(void);
	static const char* wifiPsk(void);

	/// Broker URI, e.g. "mqtt://host" or "mqtts://host:8883".
	static const char* mqttUri(void);
	/// ThingsBoard device access token, sent as the MQTT username.
	static const char* mqttToken(void);

	/// True when mqttUri() names a TLS scheme, so the cert bundle is attached.
	static bool mqttUsesTls(void);
};

#endif // _DEVICE_CONFIG_H
