#include "configs.h"

#include <Preferences.h>

#include "log.h"

namespace bb {
namespace {

constexpr int kMaxConfigs = 6;
constexpr size_t kMaxConfigBytes = 2200;
const char *kNs = "bbcfg";
const char *kIdxKey = "idx";

String sanitizeName(const String &in) {
  String s;
  for (size_t i = 0; i < in.length() && s.length() < 24; i++) {
    char c = in[i];
    if (c == '\n' || c == '\r' || c == '\t') continue;
    s += c;
  }
  s.trim();
  return s;
}

int loadNames(Preferences &p, String names[]) {
  int count = 0;
  String idx = p.getString(kIdxKey, "");
  int start = 0;
  while (start < (int)idx.length() && count < kMaxConfigs) {
    int nl = idx.indexOf('\n', start);
    if (nl < 0) nl = idx.length();
    String n = idx.substring(start, nl);
    if (n.length()) names[count++] = n;
    start = nl + 1;
  }
  return count;
}

void saveNames(Preferences &p, String names[], int count) {
  String idx;
  for (int i = 0; i < count; i++) {
    idx += names[i];
    idx += '\n';
  }
  p.putString(kIdxKey, idx);
}

String dataKey(int slot) { return String("d") + slot; }

}  // namespace

bool cfgSave(const String &rawName, const String &json, String &err) {
  String name = sanitizeName(rawName);
  if (name.isEmpty()) {
    err = "Give the config a name first.";
    return false;
  }
  if (json.length() > kMaxConfigBytes) {
    err = "That setup is too big to store on the board.";
    return false;
  }
  Preferences p;
  if (!p.begin(kNs, false)) {
    err = "Config storage is unavailable.";
    return false;
  }
  String names[kMaxConfigs];
  int count = loadNames(p, names);
  int slot = -1;
  for (int i = 0; i < count; i++)
    if (names[i].equalsIgnoreCase(name)) {
      slot = i;
      break;
    }
  if (slot < 0) {
    if (count >= kMaxConfigs) {
      p.end();
      err = "All " + String(kMaxConfigs) + " slots are full. Delete one first.";
      return false;
    }
    slot = count;
    names[count++] = name;
  }
  size_t w = p.putString(dataKey(slot).c_str(), json);
  if (w == 0) {
    p.end();
    err = "Couldn't save - not enough space on the board.";
    return false;
  }
  saveNames(p, names, count);
  p.end();
  logf("[cfg] saved \"%s\" (%u bytes, slot %d)", name.c_str(), (unsigned)json.length(), slot);
  return true;
}

bool cfgGet(const String &rawName, String &out) {
  String name = sanitizeName(rawName);
  Preferences p;
  if (!p.begin(kNs, true)) return false;
  String names[kMaxConfigs];
  int count = loadNames(p, names);
  for (int i = 0; i < count; i++) {
    if (names[i].equalsIgnoreCase(name)) {
      out = p.getString(dataKey(i).c_str(), "");
      p.end();
      return out.length() > 0;
    }
  }
  p.end();
  return false;
}

bool cfgDelete(const String &rawName, String &err) {
  String name = sanitizeName(rawName);
  Preferences p;
  if (!p.begin(kNs, false)) {
    err = "Config storage is unavailable.";
    return false;
  }
  String names[kMaxConfigs];
  int count = loadNames(p, names);
  int slot = -1;
  for (int i = 0; i < count; i++)
    if (names[i].equalsIgnoreCase(name)) {
      slot = i;
      break;
    }
  if (slot < 0) {
    p.end();
    err = "No config named that.";
    return false;
  }
  String datas[kMaxConfigs];
  for (int i = 0; i < count; i++) datas[i] = p.getString(dataKey(i).c_str(), "");
  for (int i = slot; i < count - 1; i++) {
    names[i] = names[i + 1];
    datas[i] = datas[i + 1];
  }
  count--;
  for (int i = 0; i < count; i++) p.putString(dataKey(i).c_str(), datas[i]);
  p.remove(dataKey(count).c_str());
  saveNames(p, names, count);
  p.end();
  logf("[cfg] deleted \"%s\"", name.c_str());
  return true;
}

void cfgListJson(JsonArray out) {
  Preferences p;
  if (!p.begin(kNs, true)) return;
  String names[kMaxConfigs];
  int count = loadNames(p, names);
  for (int i = 0; i < count; i++) out.add(names[i]);
  p.end();
}

}  // namespace bb
