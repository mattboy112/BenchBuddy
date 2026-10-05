#pragma once

#include <Arduino.h>
#include <ArduinoJson.h>

namespace bb {

bool i2cConfigure(int sda, int scl, uint32_t hz, String &err);
void i2cRelease();
void i2cStateJson(JsonObject out);
void i2cScan(JsonObject out);
void i2cRead(uint8_t addr, int reg, uint8_t len, JsonObject out);
void i2cWrite(uint8_t addr, const uint8_t *data, size_t len, JsonObject out);

}  // namespace bb
