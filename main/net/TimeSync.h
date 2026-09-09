#ifndef _TIME_SYNC_H
#define _TIME_SYNC_H

#include <stdint.h>
#include <time.h>

// Wall-clock time over SNTP.
//
// The device previously had no clock at all, so anything periodic could only be
// expressed as "every N hours since boot" -- which resets on every reboot and
// drifts away from anything a person would recognise. Ventilation is much
// easier to reason about as "06:00 and 18:00", and log timestamps stop being
// milliseconds-since-boot.
//
// Nothing blocks on the clock. Ventilation falls back to elapsed time until the
// first sync lands, so a dead NTP server costs accuracy, not function.

namespace TimeSync {

/// Start SNTP. Call after WiFi is set up; it does not block waiting for a fix.
void begin(void);

/// True once the system clock has been set from the network at least once.
bool isSynced(void);

/// Unix time shifted into local time, so that dividing by 86400 gives the local
/// day and the remainder gives the local time of day. Zero when unsynced.
int64_t localEpoch(void);

/// Local wall-clock time. False when unsynced, leaving `out` untouched.
bool localTime(struct tm* out);

} // namespace TimeSync

#endif // _TIME_SYNC_H
