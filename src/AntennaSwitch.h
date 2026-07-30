// Optional HTTP client for ant-sw-2x6, a separate device on the LAN: a 6
// antenna x 2 TRX switch (github.com/sq9fk/ant-sw-2x6) with its own web
// panel. This bridge does not speak OTRSP to it - that protocol stays
// reserved for the logging program (N1MM+ etc). Instead it uses the same
// plain HTTP GET requests that device's own web panel uses:
//   GET /?S{bank}{ant:02d}   select antenna 0..6 for TRX1 (bank=1) or TRX2 (bank=2)
//   GET /?J                  machine-readable status, "A=<trx1>,<trx2>"
//   GET /?K                  antenna names + site name, "K=<name1>,...,<name6>,<siteName>"
//
// Antenna names and the site name come from that device's own EEPROM (via
// /?K), not a second, independently-typed copy here - so the legend and the
// device name shown in this panel can never say something different from
// what the operator actually set on the switch.
//
// Non-blocking throughout, like the rest of this firmware: connecting to a
// misconfigured or dead host with a blocking WiFiClient could stall loop()
// for seconds, which would delay the jog dead-man timer - a real safety
// margin. So this uses AsyncClient (from AsyncTCP, already a transitive
// dependency of ESPAsyncWebServer) instead, event-driven throughout.

#pragma once

#include <Arduino.h>

namespace antswitch {

void begin();
void poll();

bool enabled();     // config.antEnabled, host non-empty
bool connected();   // last request succeeded; false while disabled

// bank: 0 = TRX1, 1 = TRX2. Returns -1 if unknown (not yet polled, or link down).
int antenna(uint8_t bank);

// index: 0..5 for antenna 1..6. Placeholder ("1".."6") until the first
// successful name fetch.
const char* antennaName(uint8_t index);

// The switch's own configured site name (its topbar label), so the operator
// can tell which physical device this panel is talking to. Empty until the
// first successful name fetch.
const char* deviceName();

// Queues one "select antenna" request; the routine status poll picks up
// right after it completes, so the panel does not wait for the next tick.
// Returns false if disabled or the arguments are out of range.
bool setAntenna(uint8_t bank, uint8_t ant);

}  // namespace antswitch
