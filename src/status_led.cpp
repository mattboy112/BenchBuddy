#include "status_led.h"

#include "config.h"

namespace bb {
namespace {

int gPin = -1;
LedState gState = LedState::Booting;
uint32_t gLastColor = 0xFFFFFFFF;

uint32_t colorFor(LedState s, uint32_t nowMs) {
  const uint8_t L = BB_STATUS_LED_LEVEL;
  bool blinkOn = (nowMs / 250) % 2 == 0;
  switch (s) {
    case LedState::Connecting: return blinkOn ? (uint32_t)L : 0;
    case LedState::Online: return (uint32_t)L << 8;
    case LedState::Hotspot: return ((uint32_t)L << 16) | (uint32_t)(L / 2);
    case LedState::Updating: return blinkOn ? (((uint32_t)L << 16) | ((uint32_t)L << 8) | L) : 0;
    case LedState::Failsafe: return blinkOn ? ((uint32_t)L << 16) : 0;
    default: return ((uint32_t)(L / 2) << 16) | ((uint32_t)(L / 2) << 8) | (L / 2);
  }
}

}  // namespace

void ledBegin(int pin) {
  gPin = pin;
  gLastColor = 0xFFFFFFFF;
}

void ledSet(LedState s) {
  gState = s;
  ledLoop(millis());
}

void ledLoop(uint32_t nowMs) {
  if (gPin < 0) return;
  uint32_t c = colorFor(gState, nowMs);
  if (c == gLastColor) return;
  gLastColor = c;
  rgbLedWrite((uint8_t)gPin, (c >> 16) & 0xFF, (c >> 8) & 0xFF, c & 0xFF);
}

}  // namespace bb
