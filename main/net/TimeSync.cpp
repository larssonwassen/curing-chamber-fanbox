#include "TimeSync.h"

#include <stdlib.h>
#include <string.h>
#include <sys/time.h>

#include "sdkconfig.h"
#include "esp_log.h"
#include "esp_netif_sntp.h"
#include "esp_sntp.h"

static const char* TAG = "time";

namespace {

// Anything before this is not a real clock: it is the epoch the RTC starts at
// when nothing has set it. 2024-01-01T00:00:00Z.
const int64_t PLAUSIBLE_EPOCH = 1704067200;

void on_sync(struct timeval* tv) {
	// Log the wall-clock time, not the delta: the point of this line is to make
	// the moment the device learned the date findable in the log stream.
	char buf[32] = {};
	struct tm local = {};
	time_t now = (time_t)tv->tv_sec;
	localtime_r(&now, &local);
	strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &local);
	ESP_LOGI(TAG, "Clock synchronised: %s local", buf);
}

} // namespace

void TimeSync::begin(void) {
	// Set the zone before the first sync so the callback above -- and every log
	// line after it -- reads in local time.
	setenv("TZ", CONFIG_TIMEZONE, 1);
	tzset();

	esp_sntp_config_t config = ESP_NETIF_SNTP_DEFAULT_CONFIG(CONFIG_NTP_SERVER);
	config.start = true;
	// Re-resolve and re-sync when the interface comes back: a router reboot
	// that changes nothing else would otherwise leave the device on a clock it
	// can no longer correct.
	config.server_from_dhcp = false;
	config.renew_servers_after_new_IP = true;
	config.index_of_first_server = 0;
	config.ip_event_to_renew = IP_EVENT_STA_GOT_IP;
	config.sync_cb = on_sync;

	esp_err_t err = esp_netif_sntp_init(&config);
	if (err != ESP_OK) {
		ESP_LOGE(TAG, "Failed to start SNTP against %s: %s",
				 CONFIG_NTP_SERVER, esp_err_to_name(err));
		return;
	}
	ESP_LOGI(TAG, "SNTP started against %s, TZ=%s", CONFIG_NTP_SERVER, CONFIG_TIMEZONE);
}

bool TimeSync::isSynced(void) {
	// Deliberately not sntp_get_sync_status(): that reports the state of the
	// most recent exchange and returns to RESET between polls, so a device that
	// synced an hour ago would look unsynced. A plausible clock is the durable
	// signal.
	return (int64_t)time(nullptr) > PLAUSIBLE_EPOCH;
}

int64_t TimeSync::localEpoch(void) {
	if (!isSynced()) {
		return 0;
	}
	time_t now = time(nullptr);
	struct tm local = {};
	localtime_r(&now, &local);
	// timegm() of the local breakdown: the same instant expressed as if local
	// time were UTC, which is what "divide by 86400 for the local day" needs.
	// Doing it this way picks up DST without hardcoding an offset.
	return (int64_t)timegm(&local);
}

bool TimeSync::localTime(struct tm* out) {
	if (!isSynced() || out == nullptr) {
		return false;
	}
	time_t now = time(nullptr);
	localtime_r(&now, out);
	return true;
}
