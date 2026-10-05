#pragma once

#include <Arduino.h>
#include <ArduinoJson.h>

namespace bb {

bool cfgSave(const String &name, const String &json, String &err);
bool cfgGet(const String &name, String &out);
bool cfgDelete(const String &name, String &err);
void cfgListJson(JsonArray out);

}  // namespace bb
