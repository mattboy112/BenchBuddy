#include "web.h"

#include <ArduinoJson.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <Update.h>
#include <WiFi.h>
#include <esp_system.h>

#include <atomic>

#include "config.h"
#include "configs.h"
#include "log.h"
#include "net.h"
#include "pins.h"
#include "web_assets.h"

namespace bb {
namespace {

AsyncWebServer gServer(80);
AsyncWebSocket gWs("/ws");
QueueHandle_t gQueue = nullptr;

std::atomic<bool> gOta{false};
std::atomic<uint32_t> gOtaLastMs{0};
std::atomic<uint32_t> gRebootAt{0};
bool gOtaDone = false;
String gOtaErr;
String gPendingImport;
String gPendingImportName;
std::atomic<bool> gImportReady{false};
uint32_t gSketchSize = 0;

constexpr uint32_t kOtaStallMs = 30000;

const char *resetReasonText(esp_reset_reason_t r) {
  switch (r) {
    case ESP_RST_POWERON: return "Power on";
    case ESP_RST_SW: return "Software restart";
    case ESP_RST_PANIC: return "Crash (panic)";
    case ESP_RST_INT_WDT:
    case ESP_RST_TASK_WDT:
    case ESP_RST_WDT: return "Watchdog";
    case ESP_RST_BROWNOUT: return "Brownout (power dipped)";
    case ESP_RST_DEEPSLEEP: return "Deep sleep wake";
    case ESP_RST_EXT: return "Reset pin";
    case ESP_RST_USB: return "USB reset";
    default: return "Other";
  }
}

void sendJson(AsyncWebServerRequest *req, int code, JsonDocument &doc) {
  String body;
  serializeJson(doc, body);
  AsyncWebServerResponse *r = req->beginResponse(code, "application/json", body);
  r->addHeader("Cache-Control", "no-store");
  req->send(r);
}

void sendResult(AsyncWebServerRequest *req, bool ok, const String &err = String()) {
  JsonDocument doc;
  doc["ok"] = ok;
  if (!ok) doc["err"] = err;
  sendJson(req, ok ? 200 : 400, doc);
}

String param(AsyncWebServerRequest *req, const char *name) {
  const AsyncWebParameter *p = req->getParam(name, true);
  if (!p) p = req->getParam(name, false);
  return p ? p->value() : String();
}

void enqueue(uint32_t client, const char *data, size_t len) {
  if (!gQueue) return;
  WsCommand cmd;
  if (len >= sizeof(cmd.data)) {
    logf("[ws] message too long (%u bytes), dropped", (unsigned)len);
    return;
  }
  cmd.client = client;
  cmd.len = (uint16_t)len;
  memcpy(cmd.data, data, len);
  cmd.data[len] = '\0';
  if (xQueueSend(gQueue, &cmd, 0) != pdTRUE) logf("[ws] command queue full, dropped");
}

void onWsEvent(AsyncWebSocket *server, AsyncWebSocketClient *client, AwsEventType type, void *arg, uint8_t *data,
               size_t len) {
  switch (type) {
    case WS_EVT_CONNECT: {
      logf("[ws] client #%lu connected from %s", (unsigned long)client->id(),
           client->remoteIP().toString().c_str());
      client->keepAlivePeriod(2);
      static const char hello[] = "{\"t\":\"hello\"}";
      enqueue(client->id(), hello, sizeof(hello) - 1);
      break;
    }
    case WS_EVT_DISCONNECT:
      logf("[ws] client #%lu disconnected", (unsigned long)client->id());
      break;
    case WS_EVT_DATA: {
      AwsFrameInfo *info = static_cast<AwsFrameInfo *>(arg);
      if (info->final && info->index == 0 && info->len == len && info->opcode == WS_TEXT) {
        enqueue(client->id(), reinterpret_cast<const char *>(data), len);
      }
      break;
    }
    default:
      break;
  }
}

void serveAsset(AsyncWebServerRequest *req, const WebAsset &a) {
  AsyncWebServerResponse *r = req->beginResponse(200, a.mime, a.data, a.len);
  if (a.gzip) r->addHeader("Content-Encoding", "gzip");
  r->addHeader("Cache-Control", a.immutable ? "public, max-age=31536000, immutable" : "no-cache");
  req->send(r);
}

void onUpdateDone(AsyncWebServerRequest *req) {
  JsonDocument doc;
  bool ok = gOtaDone && gOtaErr.isEmpty();
  doc["ok"] = ok;
  if (!ok) doc["err"] = gOtaErr.isEmpty() ? String("Upload didn't finish.") : gOtaErr;
  String body;
  serializeJson(doc, body);
  AsyncWebServerResponse *r = req->beginResponse(ok ? 200 : 500, "application/json", body);
  r->addHeader("Connection", "close");
  req->send(r);
  if (ok) {
    webScheduleReboot(1200);
  } else {
    gOta = false;
  }
}

void onUpdateChunk(AsyncWebServerRequest *req, const String &filename, size_t index, uint8_t *data, size_t len,
                   bool final) {
  gOtaLastMs = millis();
  if (index == 0) {
    gOta = true;
    gOtaDone = false;
    gOtaErr = "";
    if (Update.isRunning()) Update.abort();
    logf("[ota] receiving %s", filename.c_str());
    if (!Update.begin(UPDATE_SIZE_UNKNOWN, U_FLASH)) gOtaErr = Update.errorString();
  }
  if (gOtaErr.isEmpty() && len && Update.write(data, len) != len) {
    gOtaErr = Update.errorString();
    Update.abort();
  }
  if (final && gOtaErr.isEmpty()) {
    if (Update.end(true)) {
      gOtaDone = true;
      logf("[ota] success, %u bytes", (unsigned)(index + len));
    } else {
      gOtaErr = Update.errorString();
    }
  }
  if (!gOtaErr.isEmpty()) logf("[ota] failed: %s", gOtaErr.c_str());
}

}  // namespace

void webBuildInfo(String &out) {
  JsonDocument doc;
  doc["t"] = "hello";
  doc["fw"] = BB_FW_VERSION;
  doc["build"] = __DATE__ " " __TIME__;
  doc["chip"] = ESP.getChipModel();
  doc["rev"] = ESP.getChipRevision();
  doc["cores"] = ESP.getChipCores();
  doc["mhz"] = ESP.getCpuFreqMHz();
  doc["flash"] = ESP.getFlashChipSize();
  doc["psram"] = ESP.getPsramSize();
  doc["sketch"] = gSketchSize;
  doc["sketchMax"] = ESP.getFreeSketchSpace();
  doc["reset"] = resetReasonText(esp_reset_reason());
  pinsCatalogJson(doc.as<JsonObject>());
  netStatusJson(doc["net"].to<JsonObject>());
  serializeJson(doc, out);
}

void webBegin() {
  gQueue = xQueueCreate(16, sizeof(WsCommand));
  gSketchSize = ESP.getSketchSize();

  gWs.onEvent(onWsEvent);
  gServer.addHandler(&gWs);

  for (size_t i = 0; i < kWebAssetCount; i++) {
    gServer.on(AsyncURIMatcher::exact(kWebAssets[i].path), HTTP_GET,
               [i](AsyncWebServerRequest *req) { serveAsset(req, kWebAssets[i]); });
  }
  gServer.on(AsyncURIMatcher::exact("/index.html"), HTTP_GET,
             [](AsyncWebServerRequest *req) { serveAsset(req, kWebAssets[0]); });

  gServer.on(AsyncURIMatcher::exact("/api/info"), HTTP_GET, [](AsyncWebServerRequest *req) {
    String body;
    webBuildInfo(body);
    AsyncWebServerResponse *r = req->beginResponse(200, "application/json", body);
    r->addHeader("Cache-Control", "no-store");
    req->send(r);
  });

  gServer.on(AsyncURIMatcher::exact("/api/wifi/scan"), HTTP_GET, [](AsyncWebServerRequest *req) {
    if (req->hasParam("fresh")) netRequestScan();
    AsyncWebServerResponse *r = req->beginResponse(200, "application/json", netScanJson());
    r->addHeader("Cache-Control", "no-store");
    req->send(r);
  });

  gServer.on(AsyncURIMatcher::exact("/api/wifi/forget"), HTTP_POST, [](AsyncWebServerRequest *req) {
    netForgetWifi();
    sendResult(req, true);
    webScheduleReboot(1500);
  });

  gServer.on(AsyncURIMatcher::exact("/api/wifi"), HTTP_POST, [](AsyncWebServerRequest *req) {
    String err;
    if (!netSaveWifi(param(req, "ssid"), param(req, "pass"), param(req, "host"), err)) {
      sendResult(req, false, err);
      return;
    }
    sendResult(req, true);
    webScheduleReboot(1500);
  });

  gServer.on(AsyncURIMatcher::exact("/api/reboot"), HTTP_POST, [](AsyncWebServerRequest *req) {
    sendResult(req, true);
    webScheduleReboot(800);
  });

  gServer.on(AsyncURIMatcher::exact("/api/config/get"), HTTP_GET, [](AsyncWebServerRequest *req) {
    String name = req->hasParam("name") ? req->getParam("name")->value() : String();
    String json;
    if (!cfgGet(name, json)) {
      req->send(404, "application/json", "{\"ok\":false,\"err\":\"No config by that name\"}");
      return;
    }
    AsyncWebServerResponse *r = req->beginResponse(200, "application/json", json);
    r->addHeader("Cache-Control", "no-store");
    req->send(r);
  });

  gServer.on(AsyncURIMatcher::exact("/api/config/import"), HTTP_POST, [](AsyncWebServerRequest *req) {
    String cfg = param(req, "cfg");
    if (cfg.isEmpty()) {
      sendResult(req, false, "No config data was sent.");
      return;
    }
    if (cfg.length() > 3000) {
      sendResult(req, false, "That config file is too large.");
      return;
    }
    gPendingImport = cfg;
    gPendingImportName = param(req, "name");
    gImportReady = true;
    sendResult(req, true);
  });

  gServer.on(AsyncURIMatcher::exact("/api/update"), HTTP_POST, onUpdateDone, onUpdateChunk);

  gServer.onNotFound([](AsyncWebServerRequest *req) {
    if (netIsHotspot()) {
      const String &host = req->host();
      if (host != "192.168.4.1" && host != netHostname() + ".local") {
        req->redirect("http://192.168.4.1/");
        return;
      }
    }
    req->send(404, "application/json", "{\"ok\":false,\"err\":\"Not found\"}");
  });

  gServer.begin();
  logf("[web] server on port 80");
}

bool webNextCommand(WsCommand &out) { return gQueue && xQueueReceive(gQueue, &out, 0) == pdTRUE; }

bool webTakePendingImport(String &cfg, String &name) {
  if (!gImportReady.exchange(false)) return false;
  cfg = gPendingImport;
  name = gPendingImportName;
  gPendingImport = "";
  gPendingImportName = "";
  return true;
}

void webSendTo(uint32_t client, const String &msg) { gWs.text(client, msg); }

void webBroadcast(const String &msg) {
  if (gWs.count()) gWs.textAll(msg);
}

bool webCanStream() { return gWs.count() > 0 && gWs.availableForWriteAll(); }

size_t webClientCount() { return gWs.count(); }

void webCleanup() { gWs.cleanupClients(); }

bool webOtaActive() {
  if (gOta && !gOtaDone && millis() - gOtaLastMs.load() > kOtaStallMs) {
    logf("[ota] upload stalled, giving up");
    gOta = false;
  }
  return gOta;
}

void webScheduleReboot(uint32_t delayMs) {
  uint32_t at = millis() + delayMs;
  gRebootAt = at ? at : 1;
}

bool webRebootDue(uint32_t nowMs) {
  uint32_t at = gRebootAt.load();
  return at && (int32_t)(nowMs - at) >= 0;
}

}  // namespace bb
