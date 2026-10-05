#pragma once

#include <Arduino.h>

namespace bb {

enum class LedState : uint8_t { Booting, Connecting, Online, Hotspot, Updating, Failsafe };

void ledBegin(int pin);
void ledSet(LedState s);
void ledLoop(uint32_t nowMs);

}  // namespace bb
