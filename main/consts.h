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
	/// The filtered humidity the humidifier's raise trigger actually reads.
	/// Maintained by ClimateSensor; see climate/HumidityAverage.h. Published
	/// beside the raw value because the difference between them is the whole
	/// point: the raw trace crosses the raise level every compressor cycle and
	/// this one does not. Negative when the filter has no reading yet.
	AtomicVariable<double> humidityAverage;
	/// Humidifier fan speed, counted on its own tach line by PCNT. Zero while
	/// the fan is commanded off, which is most of the time.
	AtomicVariable<uint16_t> humidifierRPM;
	/// Percentage of the last hour the humidifier has run. Published because a
	/// humidifier sitting on its cap means the target cannot be met inside the
	/// duty the tray and the mould risk allow.
	AtomicVariable<double> humidifierDutyPercent;

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
		plateTemperature(0.0f, nullptr, onChange, this),
		humidityAverage(-1.0f, nullptr, onChange, this),
		humidifierRPM(0, nullptr, onChange, this),
		humidifierDutyPercent(0.0f, nullptr, onChange, this)
	{}
};
extern Telemetry* telemetry;

class SharedAttributes : public ChangeTrackable {
public:
	AtomicVariable<bool> fanEnabled;
	AtomicVariable<bool> ctrlLoopEnabled;
	/// Allow an OTA update to install over a development build. Off by
	/// default -- see OtaUpdater::onAttributes for why a bench build declines
	/// updates it would otherwise accept.
	AtomicVariable<bool> otaOnDevBuild;
	AtomicVariable<esp_log_level_t> uartLogLevel;
	AtomicVariable<esp_log_level_t> streamerLogLevel;
	/// Time constant, in minutes, of the humidity average, which is what the
	/// humidifier's raise trigger reads instead of the raw sensor. The
	/// chamber's own compressor swings humidity by tens of points every half
	/// hour as frost goes onto the plate and comes back off it; none of that
	/// swing means the chamber has gained or lost water. Zero reads the sensor
	/// directly. See climate/HumidityAverage.h.
	AtomicVariable<double> humidityAverageMinutes;

	// Ventilation schedule. See control/VentilationPolicy.h for what these
	// mean together; the short version is that ventilation is a timer, not a
	// controller. Humidity is not among them: the humidifier owns that now,
	// and the exchange fan runs on the clock and the plate gate alone.
	AtomicVariable<double> ventIntervalHours;
	AtomicVariable<int32_t> ventFirstHourLocal;
	AtomicVariable<double> ventBurstSeconds;
	AtomicVariable<double> ventSettleMinutes;
	AtomicVariable<double> ventMaxDeferMinutes;
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

	// The humidifier box: a fan over a tray of distilled water, ducted back
	// into the chamber. See humidifier/HumidifierPolicy.h for why its knobs are
	// separate from ventilation's rather than shared.
	//
	/// Master switch, and the whole controller while ctrl_loop_enabled is
	/// false. Off by default so a chamber with no box built yet behaves exactly
	/// as it did before this firmware shipped.
	AtomicVariable<bool> humidifierEnabled;
	/// The humidity the chamber is supposed to sit at. The only humidity
	/// setpoint in the firmware now: ventilation used to have one of its own,
	/// which asked a far weaker actuator a different question and has gone
	/// along with the coupling.
	AtomicVariable<double> humidifierTargetRh;
	/// How far below the target the average must fall before a burst starts.
	/// The burst then runs until the raw reading reaches the target, so this is
	/// the whole hysteresis band.
	AtomicVariable<double> humidifierRaiseBandRh;
	AtomicVariable<double> humidifierBurstSeconds;
	/// Quiet time after a burst. Long, because the water a burst added has to
	/// cross a duct, a chamber volume and a sensor time constant before it can
	/// be measured -- and a settle shorter than that stacks bursts on top of
	/// each other chasing a reading that has not caught up.
	AtomicVariable<double> humidifierSettleMinutes;
	/// Ceiling on the fraction of a rolling hour the fan may run. This is a
	/// mould limit, not a capacity one: a fan held over a water tray at high
	/// duty parks the chamber near saturation. Zero disables the cap.
	AtomicVariable<double> humidifierMaxDutyPercent;
	/// Edge-triggered: set it true in ThingsBoard to humidify now. Bypasses the
	/// plate gate and the duty cap, so it is also the bench test. Not persisted.
	AtomicVariable<bool> humidifyNow;

	SharedAttributes():
		ChangeTrackable(),
		fanEnabled(true, "fe", onChange, this),
		ctrlLoopEnabled(true, "cle", onChange, this),
		otaOnDevBuild(false, "odb", onChange, this),
		uartLogLevel(ESP_LOG_DEBUG, "ull", onChange, this),
		streamerLogLevel(ESP_LOG_INFO, "sll", onChange, this),
		// Comfortably longer than the measured 31-47 minute compressor cycle.
		humidityAverageMinutes(30.0, "ham", onChange, this),
		// Twice a day, anchored at 06:00 and 18:00 local.
		ventIntervalHours(12.0, "vih", onChange, this),
		ventFirstHourLocal(6, "vfh", onChange, this),
		ventBurstSeconds(90.0, "vbs", onChange, this),
		ventSettleMinutes(1.5, "vsm", onChange, this),
		ventMaxDeferMinutes(360.0, "vmd", onChange, this),
		// Matches what the default schedule already delivers (2 x 90 s), so it
		// is inert until raised.
		ventMinSecondsPerDay(180.0, "vms", onChange, this),
		ventMinDutyPercent(30, "vmp", onChange, this),
		plateGateTempC(2.0, "pgt", onChange, this),
		ventNow(false, nullptr, onChange, this),
		humidifierEnabled(false, "hen", onChange, this),
		humidifierTargetRh(78.0, "htr", onChange, this),
		humidifierRaiseBandRh(3.0, "hrb", onChange, this),
		humidifierBurstSeconds(120.0, "hbs", onChange, this),
		humidifierSettleMinutes(10.0, "hsm", onChange, this),
		humidifierMaxDutyPercent(25.0, "hmd", onChange, this),
		humidifyNow(false, nullptr, onChange, this)
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
	/// Current HumidifierState, as its underlying integer.
	AtomicVariable<int32_t> humidifierState;
	/// The humidifier reported RPM on its last sample.
	AtomicVariable<bool> humidifierRunning;
	/// Commanded on, tachometer reading zero. The fan sits in a saturated
	/// airstream over standing water, which is where a bearing goes to die.
	AtomicVariable<bool> humidifierStalled;

	Attributes():
		ChangeTrackable(),
		fanEnabled(true, nullptr, onChange, this),
		fanRunning(false, nullptr, onChange, this),
		ventState(0, nullptr, onChange, this),
		ventNextSeconds(-1, nullptr, onChange, this),
		fanStalled(false, nullptr, onChange, this),
		humidifierState(0, nullptr, onChange, this),
		humidifierRunning(false, nullptr, onChange, this),
		humidifierStalled(false, nullptr, onChange, this)
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
