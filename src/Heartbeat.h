// The onboard WS2812 (RGB_BUILTIN, GPIO47 on the LOLIN S3 Mini) doubles as a
// no-tools-needed liveness check: a dim blue pulse once a second proves the
// flash succeeded and loop() is running, even with nothing else wired up or
// no serial monitor attached. A brief green flash on every byte to or from
// the controller (RotatorLink's UART) proves that link is actually talking,
// not just that the MCU is alive - the two states share one LED because they
// answer two different "is it working?" questions at a glance.
#pragma once

namespace heartbeat {

void begin();

// Called from RotatorLink on every line sent to or received from the
// controller, regardless of config.debugEnabled - this is a physical
// indicator, not a capture, so it stays live even with Monitor/debug off.
void noteActivity();

// Non-blocking; call every loop() iteration.
void poll();

}  // namespace heartbeat
