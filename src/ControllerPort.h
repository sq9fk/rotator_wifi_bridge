// The UART to the controller, and handing it over for a controller flash.
//
// The bridge is wired to the Nano's D0/D1, which are also its USB-serial
// chip's lines. Over USB, avrdude talks to the bootloader through D0 - but our
// TX (GPIO17) idles HIGH and drives D0 directly, overpowering the CH340 behind
// its 1 kOhm resistor, and the position poller adds bytes of its own every
// 300 ms. So a controller upload fails with stk500_getsync while the bridge is
// connected.
//
// release() stops the rotator, lets the command queue drain, shuts Serial1 and
// turns GPIO17 into a high-impedance input: D0 is the CH340's until attach(),
// a reboot of the bridge, or kAutoAttachMs - a forgotten release must not
// leave rotctld/raw clients cut off for good.

#pragma once

#include <Arduino.h>
#include <ArduinoJson.h>

#include "Rotator.h"

namespace controllerport {

void begin(Rotator& rotator);
void poll();

// False if the line is already released or being released.
bool release(const char* by);
// False if the line was not released.
bool attach();

bool released();  // releasing or released - the controller is unreachable

// Adds the line state to the status's "controller" object.
void fillStatus(JsonObject controller);

}  // namespace controllerport
