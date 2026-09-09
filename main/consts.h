#ifndef _CONSTS_H
#define _CONSTS_H

#include "freertos/FreeRTOS.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "AtomicVariable/AtomicVariable.h"
#include <netdb.h>
#include <lwip/netdb.h>
#include <lwip/dns.h>
#include <lwip/sockets.h>
#include <lwip/inet.h>

/**
 * @brief Base class for classes that need change tracking via event groups
 */
class ChangeTrackable {
private:
	static const int CHANGED_BIT = BIT0;

protected:
	EventGroupHandle_t eventGroup;

	static void onChange(void* data) {
		ChangeTrackable* self = (ChangeTrackable*)data;
		ESP_LOGV("consts", "Attributes changed");
		xEventGroupSetBits(self->eventGroup, CHANGED_BIT);
	}

	ChangeTrackable(): eventGroup(xEventGroupCreate()) {
		xEventGroupSetBits(eventGroup, CHANGED_BIT);
	}

public:
	bool hasChanged(void) {
		return (xEventGroupGetBits(eventGroup) & CHANGED_BIT) != 0;
	}

	void clearChanged(void) {
		xEventGroupClearBits(eventGroup, CHANGED_BIT);
	}

	bool waitForChange(TickType_t timeout = portMAX_DELAY) {
		return (xEventGroupWaitBits(eventGroup, CHANGED_BIT, false, true, timeout) & CHANGED_BIT) != 0;
	}
};

class Telemetry : public ChangeTrackable {
public:
	AtomicVariable<uint8_t> fanDuty;
	AtomicVariable<uint16_t> fanRPM;
	AtomicVariable<double> temperature;
	AtomicVariable<double> humidity;
	/// Evaporator plate temperature. Left at its initial value, and published
	/// as null, when no probe is fitted -- see PlateProbe.
	AtomicVariable<double> plateTemperature;

	Telemetry(void):
		ChangeTrackable(),
		// Deliberately no NVS keys. These update on every sensor read (~1 Hz), and
		// persisting them wore the NVS partition past its rated ~100k erase cycles
		// per sector. Telemetry is transient by nature -- there is nothing here
		// worth restoring across a reboot. Legacy keys are purged in init_consts().
		fanDuty(0, nullptr, onChange, this),
		fanRPM(0, nullptr, onChange, this),
		temperature(0.0f, nullptr, onChange, this),
		humidity(0.0f, nullptr, onChange, this),
		plateTemperature(0.0f, nullptr, onChange, this)
	{}
};
extern Telemetry* telemetry;

class SharedAttributes : public ChangeTrackable {
public:
	AtomicVariable<bool> fanEnabled;
	AtomicVariable<bool> ctrlLoopEnabled;
	AtomicVariable<esp_log_level_t> uartLogLevel;
	AtomicVariable<esp_log_level_t> streamerLogLevel;
	AtomicVariable<double> humiditySetpoint;
	/// How far below the setpoint the chamber must fall before humidity is
	/// allowed to ask for an extra ventilation burst. There is no matching
	/// overshoot limit any more: the fan cannot dry the chamber, so there is
	/// nothing for it to do when humidity is high.
	AtomicVariable<double> humidityUndershootLimit;

	// Ventilation schedule. See control/VentilationPolicy.h for what these
	// mean together; the short version is that ventilation is a timer, not a
	// setpoint controller, because fresh air rather than humidity is its job.
	AtomicVariable<double> ventIntervalHours;
	AtomicVariable<int32_t> ventFirstHourLocal;
	AtomicVariable<double> ventBurstSeconds;
	AtomicVariable<double> ventSettleMinutes;
	AtomicVariable<double> ventMaxDeferMinutes;
	AtomicVariable<int32_t> ventMaxDryBurstsPerDay;
	/// Floor on total fan seconds per day, topped up with extra bursts spread
	/// through the day when humidity has not asked for enough air on its own.
	AtomicVariable<double> ventMinSecondsPerDay;
	/// Minimum fan duty, in percent, while a ventilation burst is running. The
	/// knob still sets the speed the rest of the time; this only stops a knob
	/// left at zero from turning a burst into silence. Zero disables it.
	AtomicVariable<int32_t> ventMinDutyPercent;
	/// Plate temperature above which ventilation may run.
	AtomicVariable<double> plateGateTempC;
	/// Edge-triggered: set it true in ThingsBoard to ventilate now. Not
	/// persisted -- a burst request should not survive a reboot.
	AtomicVariable<bool> ventNow;

	SharedAttributes():
		ChangeTrackable(),
		fanEnabled(true, "fe", onChange, this),
		ctrlLoopEnabled(true, "cle", onChange, this),
		uartLogLevel(ESP_LOG_DEBUG, "ull", onChange, this),
		streamerLogLevel(ESP_LOG_INFO, "sll", onChange, this),
		humiditySetpoint(75.0, "hsp", onChange, this),
		humidityUndershootLimit(5.0, "hus", onChange, this),
		// Twice a day, anchored at 06:00 and 18:00 local.
		ventIntervalHours(12.0, "vih", onChange, this),
		ventFirstHourLocal(6, "vfh", onChange, this),
		ventBurstSeconds(90.0, "vbs", onChange, this),
		ventSettleMinutes(20.0, "vsm", onChange, this),
		ventMaxDeferMinutes(360.0, "vmd", onChange, this),
		ventMaxDryBurstsPerDay(6, "mdb", onChange, this),
		// Matches what the default schedule already delivers (2 x 90 s), so it
		// is inert until raised.
		ventMinSecondsPerDay(180.0, "vms", onChange, this),
		ventMinDutyPercent(30, "vmp", onChange, this),
		plateGateTempC(2.0, "pgt", onChange, this),
		ventNow(false, nullptr, onChange, this)
	{}
};
extern SharedAttributes* shared_attributes;

class Attributes : public ChangeTrackable {
public:
	AtomicVariable<bool> fanEnabled;
	AtomicVariable<bool> fanRunning;
	/// Current VentState, as its underlying integer. Stored rather than
	/// recomputed so the publisher does not need the policy object.
	AtomicVariable<int32_t> ventState;
	/// Seconds until the next scheduled burst. -1 when one is due or running.
	AtomicVariable<int32_t> ventNextSeconds;
	/// The fan was told to run during a burst and reported no RPM. A burst that
	/// moves no air is indistinguishable from one that worked, unless this is
	/// reported.
	AtomicVariable<bool> fanStalled;

	Attributes():
		ChangeTrackable(),
		fanEnabled(true, nullptr, onChange, this),
		fanRunning(false, nullptr, onChange, this),
		ventState(0, nullptr, onChange, this),
		ventNextSeconds(-1, nullptr, onChange, this),
		fanStalled(false, nullptr, onChange, this)
	{}
};
extern Attributes* attributes;

#define MAX(x, y) ((x) > (y) ? (x) : (y))
#define MIN(x, y) ((x) > (y) ? (y) : (x))

#ifndef INET_ADDRSTRLEN
#define INET_ADDRSTRLEN 16
#endif
#ifndef INET6_ADDRSTRLEN
#define INET6_ADDRSTRLEN 46
#endif

extern EventGroupHandle_t network_state_event_group;
extern const int WIFI_CONNECTED_BIT;
extern const int MQTT_CONNECTED_BIT;


void init_consts(void);
void log_nvs_stats(const char* when);
void getMAC(char* out);
void print_ip_info(const char* tag);
void dns_debug(const char *host, const char *service);
void waitForBit(int bit);
/// Bounded variant. Returns false if the bit did not appear within the timeout.
bool waitForBit(int bit, TickType_t timeout_ticks);

#endif // _CONSTS_H
