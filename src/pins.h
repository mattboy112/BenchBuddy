#pragma once

#include <Arduino.h>
#include <ArduinoJson.h>

namespace bb {

enum class Owner : uint8_t { User, I2C };

void pinsBegin(int statusLedPin);
bool pinsCommand(JsonObjectConst cmd, String &err);
void pinsAllOff();
bool pinsHasActiveOutputs();

void pinsLoop(uint32_t nowMs);
void pinsSample();
uint16_t pinsSampleHz();
bool pinsSetSampleHz(uint16_t hz);
bool pinsHasAnalog();

bool pinsIsUserPin(uint8_t gpio);
bool pinsClaim(uint8_t gpio, Owner owner, String &err);
void pinsRelease(uint8_t gpio, Owner owner);

void pinsConfigJson(JsonObject out);
bool pinsApplyConfig(JsonObjectConst cfg, String &err);
void pinsSuspend();
bool pinsResume(String &err);
bool pinsSuspended();
void pinsCancelSuspend();

void pinsCatalogJson(JsonObject out);
void pinsStateJson(JsonArray out);
void pinsLiveJson(JsonObject out);

}  // namespace bb
