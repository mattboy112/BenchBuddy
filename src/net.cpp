#include "net.h"

#include <DNSServer.h>
#include <ESPmDNS.h>
#include <Preferences.h>
#include <WiFi.h>
#include <esp_mac.h>

#include <algorithm>
#include <atomic>
#include <mutex>
#include <vector>

#include "config.h"
#include "log.h"
#include "status_led.h"

namespace bb {
namespace {

enum class NetState : uint8_t { Connecting, Online, Hotspot };

constexpr const char *kPrefsNs = "bench";

NetState gState = NetState::Connecting;
String gSsid;
String gPass;
String gHost;
String gApSsid;
bool gFailsafe = false;
int gLedPin = BB_STATUS_LED_PIN;
bool gSettingsLoaded = false;

DNSServer gDns;
bool gDnsOn = false;
bool gLinkUp = false;
uint32_t gLostSince = 0;
uint32_t gLastRetry = 0;
uint32_t gRetryStart = 0;
bool gRetrying = false;

std::mutex gScanMutex;
std::atomic<bool> gScanRequested{false};
bool gScanRunning = false;
String gScanResult = "{\"state\":\"idle\",\"nets\":[]}";

String sanitizeHost(const String &in) {
  String out;
  for (size_t i = 0; i < in.length() && out.length() < 31; i++) {
    char c = (char)tolower((unsigned char)in[i]);
    if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-') out += c;
  }
  while (out.startsWith("-")) out.remove(0, 1);
  while (out.endsWith("-")) out.remove(out.length() - 1);
  return out;
}

void loadSettings() {
  Preferences p;
  p.begin(kPrefsNs, true);
  gSsid = p.getString("ssid", BB_DEFAULT_WIFI_SSID);
  gPass = p.getString("pass", BB_DEFAULT_WIFI_PASS);
  gHost = sanitizeHost(p.getString("host", BB_DEFAULT_HOSTNAME));
  gFailsafe = p.getBool("fs", false);
  gLedPin = p.getInt("led", BB_STATUS_LED_PIN);
  p.end();
  if (gLedPin != -1 && gLedPin != 38 && gLedPin != 48) gLedPin = BB_STATUS_LED_PIN;
  if (gHost.isEmpty()) gHost = BB_DEFAULT_HOSTNAME;

  uint8_t mac[6] = {};
  esp_efuse_mac_get_default(mac);
  char suffix[5];
  snprintf(suffix, sizeof(suffix), "%02X%02X", mac[4], mac[5]);
  gApSsid = String(BB_AP_SSID_PREFIX) + suffix;
}

void startMdns() {
  MDNS.end();
  if (MDNS.begin(gHost.c_str())) {
    MDNS.addService("http", "tcp", 80);
    logf("[net] mDNS: http://%s.local", gHost.c_str());
  } else {
    logf("[net] mDNS failed to start");
  }
}

bool connectBlocking() {
  logf("[net] connecting to \"%s\"...", gSsid.c_str());
  WiFi.begin(gSsid.c_str(), gPass.c_str());
  uint32_t t0 = millis();
  while (millis() - t0 < BB_STA_CONNECT_TIMEOUT_MS) {
    if (WiFi.status() == WL_CONNECTED) return true;
    ledLoop(millis());
    delay(50);
  }
  return false;
}

void startHotspot() {
  WiFi.setAutoReconnect(false);
  WiFi.disconnect(false, false);
  WiFi.mode(WIFI_AP_STA);
  WiFi.softAPConfig(IPAddress(192, 168, 4, 1), IPAddress(192, 168, 4, 1), IPAddress(255, 255, 255, 0));
  WiFi.softAP(gApSsid.c_str(), BB_AP_PASSWORD);
  gDns.setErrorReplyCode(DNSReplyCode::NoError);
  gDnsOn = gDns.start(53, "*", WiFi.softAPIP());
  gState = NetState::Hotspot;
  gRetrying = false;
  gLastRetry = millis();
  ledSet(LedState::Hotspot);
  startMdns();
  logf("[net] hotspot \"%s\" (password \"%s\") at http://192.168.4.1", gApSsid.c_str(), BB_AP_PASSWORD);
}

void becomeOnline() {
  if (gDnsOn) {
    gDns.stop();
    gDnsOn = false;
  }
  if (WiFi.getMode() & WIFI_AP) WiFi.softAPdisconnect(false);
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  gState = NetState::Online;
  gLinkUp = true;
  gLostSince = 0;
  ledSet(LedState::Online);
  startMdns();
  logf("[net] online: \"%s\" IP %s RSSI %d dBm", WiFi.SSID().c_str(), WiFi.localIP().toString().c_str(),
       (int)WiFi.RSSI());
}

void finishScan(int16_t n) {
  JsonDocument doc;
  if (n < 0) {
    doc["state"] = "error";
    doc["nets"].to<JsonArray>();
  } else {
    struct Net {
      String ssid;
      int32_t rssi;
      bool open;
      int32_t ch;
    };
    std::vector<Net> nets;
    for (int16_t i = 0; i < n; i++) {
      String ssid = WiFi.SSID(i);
      if (ssid.isEmpty()) continue;
      int32_t rssi = WiFi.RSSI(i);
      auto it = std::find_if(nets.begin(), nets.end(), [&](const Net &x) { return x.ssid == ssid; });
      if (it != nets.end()) {
        if (rssi > it->rssi) it->rssi = rssi;
        continue;
      }
      nets.push_back({ssid, rssi, WiFi.encryptionType(i) == WIFI_AUTH_OPEN, WiFi.channel(i)});
    }
    std::sort(nets.begin(), nets.end(), [](const Net &a, const Net &b) { return a.rssi > b.rssi; });
    doc["state"] = "done";
    JsonArray arr = doc["nets"].to<JsonArray>();
    for (size_t i = 0; i < nets.size() && i < 25; i++) {
      JsonObject o = arr.add<JsonObject>();
      o["ssid"] = nets[i].ssid;
      o["rssi"] = nets[i].rssi;
      o["open"] = nets[i].open;
      o["ch"] = nets[i].ch;
    }
  }
  String json;
  serializeJson(doc, json);
  std::lock_guard<std::mutex> lock(gScanMutex);
  gScanResult = json;
}

void scanLoop() {
  if (gScanRunning) {
    int16_t n = WiFi.scanComplete();
    if (n == WIFI_SCAN_RUNNING) return;
    finishScan(n);
    WiFi.scanDelete();
    gScanRunning = false;
    return;
  }
  if (!gScanRequested.load() || gRetrying) return;
  gScanRequested = false;
  int16_t r = WiFi.scanNetworks(true, false);
  if (r == WIFI_SCAN_FAILED) {
    finishScan(-1);
    return;
  }
  gScanRunning = true;
  std::lock_guard<std::mutex> lock(gScanMutex);
  gScanResult = "{\"state\":\"scanning\",\"nets\":[]}";
}

}  // namespace

void netLoadSettings() {
  if (gSettingsLoaded) return;
  loadSettings();
  gSettingsLoaded = true;
}

void netBegin() {
  netLoadSettings();
  WiFi.persistent(false);
  WiFi.setHostname(gHost.c_str());
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  ledSet(LedState::Connecting);

  if (gSsid.length() && connectBlocking()) {
    becomeOnline();
  } else {
    if (gSsid.length()) logf("[net] couldn't reach \"%s\", starting hotspot", gSsid.c_str());
    startHotspot();
  }
}

void netLoop(uint32_t nowMs) {
  scanLoop();

  if (gState == NetState::Online) {
    bool up = WiFi.status() == WL_CONNECTED;
    if (up) {
      if (!gLinkUp) {
        gLinkUp = true;
        ledSet(LedState::Online);
        logf("[net] reconnected, IP %s", WiFi.localIP().toString().c_str());
      }
      gLostSince = 0;
    } else {
      if (gLinkUp) {
        gLinkUp = false;
        gLostSince = nowMs;
        ledSet(LedState::Connecting);
        logf("[net] WiFi lost, retrying");
      }
      if (nowMs - gLostSince > BB_STA_LOST_TO_AP_MS) {
        logf("[net] WiFi still down, falling back to hotspot");
        startHotspot();
      }
    }
    return;
  }

  if (gState != NetState::Hotspot || gSsid.isEmpty()) return;
  if (gRetrying) {
    if (WiFi.status() == WL_CONNECTED) {
      gRetrying = false;
      becomeOnline();
    } else if (nowMs - gRetryStart > BB_STA_CONNECT_TIMEOUT_MS) {
      WiFi.disconnect(false, false);
      gRetrying = false;
      gLastRetry = nowMs;
    }
  } else if (nowMs - gLastRetry > BB_STA_RETRY_INTERVAL_MS && WiFi.softAPgetStationNum() == 0 && !gScanRunning) {
    gRetrying = true;
    gRetryStart = nowMs;
    WiFi.begin(gSsid.c_str(), gPass.c_str());
  }
}

bool netIsHotspot() { return gState == NetState::Hotspot; }

String netHostname() { return gHost; }

void netStatusJson(JsonObject out) {
  bool hotspot = gState == NetState::Hotspot;
  out["mode"] = hotspot ? "hotspot" : (WiFi.status() == WL_CONNECTED ? "wifi" : "connecting");
  out["host"] = gHost;
  out["apSsid"] = gApSsid;
  out["saved"] = gSsid;
  out["mac"] = WiFi.macAddress();
  if (hotspot) {
    out["ssid"] = gApSsid;
    out["ip"] = WiFi.softAPIP().toString();
    out["stations"] = WiFi.softAPgetStationNum();
  } else {
    out["ssid"] = WiFi.SSID();
    out["ip"] = WiFi.localIP().toString();
    out["rssi"] = WiFi.RSSI();
    out["ch"] = WiFi.channel();
  }
}

bool netSaveWifi(const String &ssid, const String &pass, const String &host, String &err) {
  if (ssid.isEmpty() || ssid.length() > 32) {
    err = "Network name must be 1-32 characters.";
    return false;
  }
  if (!pass.isEmpty() && (pass.length() < 8 || pass.length() > 63)) {
    err = "WiFi passwords are 8-63 characters (or leave it blank for an open network).";
    return false;
  }
  String cleanHost = sanitizeHost(host.isEmpty() ? gHost : host);
  if (cleanHost.isEmpty()) {
    err = "Device name can use letters, numbers and dashes.";
    return false;
  }
  Preferences p;
  if (!p.begin(kPrefsNs, false)) {
    err = "Couldn't open settings storage.";
    return false;
  }
  p.putString("ssid", ssid);
  p.putString("pass", pass);
  p.putString("host", cleanHost);
  p.end();
  logf("[net] saved WiFi \"%s\", device name \"%s\"", ssid.c_str(), cleanHost.c_str());
  return true;
}

void netForgetWifi() {
  Preferences p;
  if (p.begin(kPrefsNs, false)) {
    p.remove("ssid");
    p.remove("pass");
    p.end();
  }
  logf("[net] WiFi forgotten");
}

void netRequestScan() {
  std::lock_guard<std::mutex> lock(gScanMutex);
  gScanResult = "{\"state\":\"scanning\",\"nets\":[]}";
  gScanRequested = true;
}

String netScanJson() {
  std::lock_guard<std::mutex> lock(gScanMutex);
  return gScanResult;
}

int netLedPin() { return gLedPin; }

bool netSetLedPin(int pin) {
  if (pin != -1 && pin != 38 && pin != 48) return false;
  Preferences p;
  if (!p.begin(kPrefsNs, false)) return false;
  p.putInt("led", pin);
  p.end();
  gLedPin = pin;
  logf("[net] status LED pin set to %d", pin);
  return true;
}

bool netFailsafeEnabled() { return gFailsafe; }

void netSetFailsafe(bool on) {
  if (on == gFailsafe) return;
  gFailsafe = on;
  Preferences p;
  if (p.begin(kPrefsNs, false)) {
    p.putBool("fs", on);
    p.end();
  }
}

}  // namespace bb
