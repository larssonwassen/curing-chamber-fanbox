#include "DeviceConfig.h"

#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

#include <string.h>

static const char* TAG = "config";

// Sized to the protocol maxima plus a terminator: SSID 32, WPA2/3 passphrase
// 63. The URI and token bounds are ours.
static char s_wifi_ssid[33];
static char s_wifi_psk[64];
static char s_mqtt_uri[128];
static char s_mqtt_token[65];
static bool s_loaded = false;

namespace {

struct Entry {
	const char* key;
	char* dest;
	size_t cap;
};

const Entry kEntries[] = {
	{ "wifi_ssid", s_wifi_ssid,  sizeof(s_wifi_ssid)  },
	{ "wifi_psk",  s_wifi_psk,   sizeof(s_wifi_psk)   },
	{ "tb_uri",    s_mqtt_uri,   sizeof(s_mqtt_uri)   },
	{ "tb_token",  s_mqtt_token, sizeof(s_mqtt_token) },
};

} // namespace

esp_err_t DeviceConfig::load(void) {
	s_loaded = false;

	nvs_handle_t handle;
	esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle);
	if (err != ESP_OK) {
		ESP_LOGE(TAG, "nvs_open('%s') failed: %s", NVS_NAMESPACE, esp_err_to_name(err));
		return err;
	}

	// Report every problem in one pass rather than stopping at the first, so a
	// half-provisioned device does not need four reboots to enumerate.
	esp_err_t first_error = ESP_OK;
	for (size_t i = 0; i < sizeof(kEntries) / sizeof(kEntries[0]); i++) {
		const Entry& e = kEntries[i];
		e.dest[0] = '\0';

		size_t len = e.cap;
		esp_err_t get_err = nvs_get_str(handle, e.key, e.dest, &len);
		if (get_err != ESP_OK) {
			ESP_LOGE(TAG, "Missing or unreadable key '%s': %s", e.key, esp_err_to_name(get_err));
			e.dest[0] = '\0';
			if (first_error == ESP_OK) {
				first_error = get_err;
			}
			continue;
		}
		if (e.dest[0] == '\0') {
			ESP_LOGE(TAG, "Key '%s' is present but empty", e.key);
			if (first_error == ESP_OK) {
				first_error = ESP_ERR_INVALID_STATE;
			}
		}
	}
	nvs_close(handle);

	if (first_error != ESP_OK) {
		return first_error;
	}

	// Log the endpoints, never the secrets.
	ESP_LOGI(TAG, "Provisioned: ssid='%s' broker='%s' (tls=%s)",
			 s_wifi_ssid, s_mqtt_uri, mqttUsesTls() ? "yes" : "no");
	s_loaded = true;
	return ESP_OK;
}

bool DeviceConfig::isLoaded(void) { return s_loaded; }

const char* DeviceConfig::wifiSsid(void)  { return s_wifi_ssid; }
const char* DeviceConfig::wifiPsk(void)   { return s_wifi_psk; }
const char* DeviceConfig::mqttUri(void)   { return s_mqtt_uri; }
const char* DeviceConfig::mqttToken(void) { return s_mqtt_token; }

bool DeviceConfig::mqttUsesTls(void) {
	return strncmp(s_mqtt_uri, "mqtts://", 8) == 0 ||
		   strncmp(s_mqtt_uri, "wss://", 6) == 0;
}
