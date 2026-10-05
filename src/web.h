#pragma once

#include <Arduino.h>

namespace bb {

struct WsCommand {
  uint32_t client;
  uint16_t len;
  char data[512];
};

void webBegin();
bool webNextCommand(WsCommand &out);
void webSendTo(uint32_t client, const String &msg);
void webBroadcast(const String &msg);
bool webCanStream();
size_t webClientCount();
void webCleanup();

bool webOtaActive();
bool webRebootDue(uint32_t nowMs);
void webScheduleReboot(uint32_t delayMs);

void webBuildInfo(String &out);

}  // namespace bb
