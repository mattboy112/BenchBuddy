#pragma once

#include <Arduino.h>
#include <ArduinoJson.h>

namespace bb {

void netLoadSettings();
void netBegin();
void netLoop(uint32_t nowMs);

bool netIsHotspot();
String netHostname();
void netStatusJson(JsonObject out);

bool netSaveWifi(const String &ssid, const String &pass, const String &host, String &err);
void netForgetWifi();

void netRequestScan();
String netScanJson();

int netLedPin();
bool netSetLedPin(int pin);

bool netFailsafeEnabled();
void netSetFailsafe(bool on);

}  // namespace bb
