#include <Arduino.h>
#include <ArduinoJson.h>

#include "config.h"
#include "configs.h"
#include "i2c_tools.h"
#include "log.h"
#include "net.h"
#include "pins.h"
#include "status_led.h"
#include "web.h"

using namespace bb;

namespace {

constexpr uint32_t kLiveMs = 50;
constexpr uint32_t kSysMs = 2000;
constexpr uint32_t kHousekeepingMs = 1000;
constexpr int kMaxCommandsPerLoop = 8;

uint32_t gNextSampleUs = 0;
uint32_t gLastLive = 0;
uint32_t gLastSys = 0;
uint32_t gLastHousekeeping = 0;
uint32_t gNoClientSince = 0;
bool gStateDirty = true;
bool gOtaHandled = false;
bool gFailsafeTripped = false;
String gNotice;

void send(uint32_t client, JsonDocument &doc) {
  String out;
  serializeJson(doc, out);
  if (client) {
    webSendTo(client, out);
  } else {
    webBroadcast(out);
  }
}

void sendError(uint32_t client, const String &msg, JsonVariantConst id) {
  JsonDocument doc;
  doc["t"] = "err";
  doc["msg"] = msg;
  if (!id.isNull()) doc["id"] = id;
  send(client, doc);
  logf("[cmd] error: %s", msg.c_str());
}

String stateJson() {
  JsonDocument doc;
  doc["t"] = "state";
  pinsStateJson(doc["pins"].to<JsonArray>());
  i2cStateJson(doc["i2c"].to<JsonObject>());
  doc["rate"] = pinsSampleHz();
  doc["fs"] = netFailsafeEnabled();
  doc["susp"] = pinsSuspended();
  cfgListJson(doc["cfgs"].to<JsonArray>());
  String out;
  serializeJson(doc, out);
  return out;
}

String sysJson() {
  JsonDocument doc;
  doc["t"] = "sys";
  doc["up"] = millis();
  doc["heap"] = ESP.getFreeHeap();
  doc["heapMin"] = ESP.getMinFreeHeap();
  doc["heapSize"] = ESP.getHeapSize();
  doc["psramFree"] = ESP.getFreePsram();
  float temp = temperatureRead();
  if (isfinite(temp)) doc["temp"] = round((double)temp * 10.0) / 10.0;
  doc["clients"] = webClientCount();
  netStatusJson(doc["net"].to<JsonObject>());
  String out;
  serializeJson(doc, out);
  return out;
}

bool readAddress(JsonDocument &doc, uint8_t &addr) {
  int a = doc["a"] | -1;
  if (a < 0x08 || a > 0x77) return false;
  addr = (uint8_t)a;
  return true;
}

void handleCommand(const WsCommand &c) {
  JsonDocument doc;
  DeserializationError de = deserializeJson(doc, c.data, c.len);
  if (de) {
    sendError(c.client, "Couldn't read that command.", JsonVariantConst());
    return;
  }
  const char *t = doc["t"] | "";
  JsonVariantConst id = doc["id"];
  String err;

  if (strcmp(t, "hello") == 0) {
    String info;
    webBuildInfo(info);
    webSendTo(c.client, info);
    webSendTo(c.client, stateJson());
    webSendTo(c.client, sysJson());
    if (gNotice.length()) {
      JsonDocument n;
      n["t"] = "notice";
      n["msg"] = gNotice;
      send(c.client, n);
      gNotice = "";
    }
    if (gFailsafeTripped) {
      gFailsafeTripped = false;
      ledSet(netIsHotspot() ? LedState::Hotspot : LedState::Online);
    }
    return;
  }

  if (strcmp(t, "ping") == 0) return;

  if (strcmp(t, "pin") == 0) {
    if (webOtaActive()) {
      sendError(c.client, "A firmware update is running. Pins stay off until it finishes.", id);
      gStateDirty = true;
      return;
    }
    if (!pinsCommand(doc.as<JsonObjectConst>(), err)) sendError(c.client, err, id);
    else pinsCancelSuspend();
    gStateDirty = true;
    return;
  }

  if (strcmp(t, "alloff") == 0) {
    pinsAllOff();
    gStateDirty = true;
    logf("[cmd] all outputs off");
    return;
  }

  if (strcmp(t, "suspend") == 0) {
    if (webOtaActive()) {
      sendError(c.client, "A firmware update is running.", id);
      return;
    }
    pinsSuspend();
    gStateDirty = true;
    logf("[cmd] suspended");
    return;
  }

  if (strcmp(t, "resume") == 0) {
    if (!pinsResume(err)) sendError(c.client, err, id);
    gStateDirty = true;
    logf("[cmd] resumed");
    return;
  }

  if (strcmp(t, "cfg_save") == 0) {
    JsonDocument cfg;
    pinsConfigJson(cfg.to<JsonObject>());
    String json;
    serializeJson(cfg, json);
    if (!cfgSave(doc["name"] | "", json, err)) sendError(c.client, err, id);
    gStateDirty = true;
    return;
  }

  if (strcmp(t, "cfg_apply") == 0) {
    String json;
    if (!cfgGet(doc["name"] | "", json)) {
      sendError(c.client, "Couldn't find that config.", id);
      return;
    }
    JsonDocument cfg;
    if (deserializeJson(cfg, json)) {
      sendError(c.client, "That saved config is corrupted.", id);
      return;
    }
    if (!pinsApplyConfig(cfg.as<JsonObjectConst>(), err))
      sendError(c.client, "Loaded, but a pin was skipped: " + err, id);
    gStateDirty = true;
    return;
  }

  if (strcmp(t, "cfg_delete") == 0) {
    if (!cfgDelete(doc["name"] | "", err)) sendError(c.client, err, id);
    gStateDirty = true;
    return;
  }

  if (strcmp(t, "rate") == 0) {
    int hz = doc["hz"] | 0;
    if (hz <= 0 || !pinsSetSampleHz((uint16_t)hz)) sendError(c.client, "Pick 10, 20, 50, 100 or 200 samples per second.", id);
    gNextSampleUs = micros();
    gStateDirty = true;
    return;
  }

  if (strcmp(t, "led") == 0) {
    int pin = doc["pin"] | -2;
    if (pin != -1 && pin != 38 && pin != 48) {
      sendError(c.client, "Pick GPIO 38, GPIO 48 or none.", id);
      return;
    }
    if (pin == netLedPin()) return;
    if (!netSetLedPin(pin)) {
      sendError(c.client, "Couldn't save the LED setting.", id);
      return;
    }
    JsonDocument n;
    n["t"] = "notice";
    n["msg"] = pin < 0 ? String("Status LED turned off. Restarting to apply.")
                       : "Status LED moved to GPIO " + String(pin) + ". Restarting to apply.";
    send(0, n);
    webScheduleReboot(1500);
    return;
  }

  if (strcmp(t, "failsafe") == 0) {
    netSetFailsafe(doc["on"] | false);
    gStateDirty = true;
    return;
  }

  if (strcmp(t, "i2c_cfg") == 0) {
    int hz = doc["hz"] | 100000;
    if (!i2cConfigure(doc["sda"] | -1, doc["scl"] | -1, hz > 0 ? (uint32_t)hz : 0, err)) sendError(c.client, err, id);
    gStateDirty = true;
    return;
  }

  if (strcmp(t, "i2c_off") == 0) {
    i2cRelease();
    gStateDirty = true;
    return;
  }

  if (strcmp(t, "i2c_scan") == 0) {
    JsonDocument r;
    r["t"] = "i2c_scan";
    if (!id.isNull()) r["id"] = id;
    i2cScan(r.as<JsonObject>());
    send(0, r);
    gStateDirty = true;
    return;
  }

  if (strcmp(t, "i2c_read") == 0 || strcmp(t, "i2c_write") == 0) {
    uint8_t addr;
    if (!readAddress(doc, addr)) {
      sendError(c.client, "Pick an address between 0x08 and 0x77.", id);
      return;
    }
    JsonDocument r;
    r["t"] = "i2c_rw";
    if (!id.isNull()) r["id"] = id;
    if (t[4] == 'r') {
      int reg = doc["r"] | -1;
      if (reg > 255) reg = 255;
      if (reg < -1) reg = -1;
      int n = doc["n"] | 1;
      i2cRead(addr, reg, (uint8_t)constrain(n, 1, 32), r.as<JsonObject>());
    } else {
      uint8_t bytes[32];
      size_t count = 0;
      for (JsonVariantConst v : doc["b"].as<JsonArrayConst>()) {
        int b = v | -1;
        if (b < 0 || b > 255 || count >= sizeof(bytes)) {
          sendError(c.client, "Write needs 1-32 bytes, each 00-FF.", id);
          return;
        }
        bytes[count++] = (uint8_t)b;
      }
      if (!count) {
        sendError(c.client, "Write needs 1-32 bytes, each 00-FF.", id);
        return;
      }
      i2cWrite(addr, bytes, count, r.as<JsonObject>());
    }
    send(c.client, r);
    gStateDirty = true;
    return;
  }

  sendError(c.client, String("Unknown command \"") + t + "\".", id);
}

void sampleIfDue() {
  uint32_t period = 1000000UL / pinsSampleHz();
  uint32_t us = micros();
  if ((int32_t)(us - gNextSampleUs) < 0) return;
  pinsSample();
  gNextSampleUs += period;
  if ((int32_t)(us - gNextSampleUs) > (int32_t)(period * 4)) gNextSampleUs = us + period;
}

void housekeeping(uint32_t now) {
  webCleanup();
  if (webClientCount() > 0) {
    gNoClientSince = 0;
    return;
  }
  if (!gNoClientSince) gNoClientSince = now ? now : 1;
  if (netFailsafeEnabled() && now - gNoClientSince >= BB_FAILSAFE_TIMEOUT_MS && pinsHasActiveOutputs()) {
    pinsAllOff();
    gStateDirty = true;
    gNotice = "Failsafe switched every output off because no browser was connected for 10 seconds.";
    gFailsafeTripped = true;
    ledSet(LedState::Failsafe);
    logf("[failsafe] no clients for %lu ms, outputs off", (unsigned long)(now - gNoClientSince));
  }
}

}  // namespace

void setup() {
  logBegin();
  delay(300);
  logf("BenchBuddy %s (%s %s)", BB_FW_VERSION, __DATE__, __TIME__);
  netLoadSettings();
  ledBegin(netLedPin());
  ledSet(LedState::Booting);
  pinsBegin(netLedPin());
  netBegin();
  webBegin();
  gNextSampleUs = micros();
  logf("[boot] ready, free heap %lu bytes, PSRAM %lu bytes", (unsigned long)ESP.getFreeHeap(),
       (unsigned long)ESP.getPsramSize());
}

void loop() {
  uint32_t now = millis();
  netLoop(now);

  static WsCommand cmd;
  for (int i = 0; i < kMaxCommandsPerLoop && webNextCommand(cmd); i++) handleCommand(cmd);

  {
    String cfgData, cfgSaveName;
    if (webTakePendingImport(cfgData, cfgSaveName)) {
      JsonDocument cfg;
      JsonDocument n;
      n["t"] = "notice";
      if (deserializeJson(cfg, cfgData)) {
        n["msg"] = "That imported config file couldn't be read.";
      } else {
        String e;
        pinsApplyConfig(cfg.as<JsonObjectConst>(), e);
        if (cfgSaveName.length()) {
          String e2;
          cfgSave(cfgSaveName, cfgData, e2);
        }
        n["msg"] = "Imported config applied.";
      }
      send(0, n);
      gStateDirty = true;
    }
  }

  bool ota = webOtaActive();
  if (ota && !gOtaHandled) {
    gOtaHandled = true;
    pinsAllOff();
    gStateDirty = true;
    ledSet(LedState::Updating);
    logf("[ota] outputs off for update");
  } else if (!ota && gOtaHandled) {
    gOtaHandled = false;
    ledSet(netIsHotspot() ? LedState::Hotspot : LedState::Online);
  }

  sampleIfDue();
  pinsLoop(now);

  if (now - gLastLive >= kLiveMs) {
    gLastLive = now;
    if (!ota && webCanStream()) {
      JsonDocument doc;
      doc["t"] = "live";
      doc["up"] = now;
      doc["hz"] = pinsSampleHz();
      pinsLiveJson(doc.as<JsonObject>());
      send(0, doc);
    }
  }

  if (gStateDirty) {
    gStateDirty = false;
    webBroadcast(stateJson());
  }

  if (now - gLastSys >= kSysMs) {
    gLastSys = now;
    if (webClientCount()) webBroadcast(sysJson());
  }

  if (now - gLastHousekeeping >= kHousekeepingMs) {
    gLastHousekeeping = now;
    housekeeping(now);
  }

  ledLoop(now);

  if (webRebootDue(now)) {
    logf("[boot] restarting");
    pinsAllOff();
    delay(200);
    ESP.restart();
  }

  delay(1);
}
