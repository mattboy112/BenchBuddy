#include "pins.h"

#include <driver/gpio.h>
#include <driver/pulse_cnt.h>
#include <math.h>

#include "config.h"
#include "log.h"

namespace bb {
namespace {

enum class Mode : uint8_t { Off, Input, Output, Pwm, Servo, Analog };
enum class Pull : uint8_t { None, Up, Down };

constexpr uint8_t kCandidatePins[] = {1,  2,  4,  5,  6,  7,  8,  9,  10, 11, 12, 13, 14,
                                      15, 16, 17, 18, 21, 38, 39, 40, 41, 42, 47, 48};
constexpr size_t kMaxPins = sizeof(kCandidatePins);
constexpr uint8_t kMaxAnalog = 4;
constexpr uint8_t kMaxCounters = 4;
constexpr int kCounterLimit = 30000;
constexpr uint8_t kSampleBuf = 48;
constexpr uint8_t kRateWindow = 11;
constexpr uint8_t kAdcOversample = 4;
constexpr uint32_t kPwmFreqMin = 5;
constexpr uint32_t kPwmFreqMax = 100000;
constexpr uint32_t kServoHz = 50;
constexpr uint8_t kServoBits = 14;
constexpr uint32_t kServoPeriodUs = 1000000UL / kServoHz;
constexpr uint16_t kServoAbsMin = 400;
constexpr uint16_t kServoAbsMax = 2600;
constexpr uint16_t kRates[] = {10, 20, 50, 100, 200};

struct Counter {
  bool used = false;
  pcnt_unit_handle_t unit = nullptr;
  pcnt_channel_handle_t chan = nullptr;
};

struct RateSnap {
  uint32_t ms;
  uint32_t count;
};

struct Pin {
  uint8_t gpio = 0;
  bool adc = false;
  Owner owner = Owner::User;
  Mode mode = Mode::Off;
  Pull pull = Pull::None;
  bool level = false;
  uint32_t freq = 1000;
  uint32_t freqActual = 0;
  double duty = 50.0;
  uint8_t res = 0;
  uint16_t us = 1500;
  uint16_t usMin = 500;
  uint16_t usMax = 2500;
  uint16_t usNow = 1500;
  bool sweep = false;
  uint16_t sweepMs = 2000;
  double scale = 1.0;
  int8_t counter = -1;
  RateSnap snaps[kRateWindow] = {};
  uint8_t snapHead = 0;
  uint8_t snapCount = 0;
  float hz = 0.0f;
  uint16_t samples[kSampleBuf] = {};
  uint8_t sampleCount = 0;
};

Pin gPins[kMaxPins];
size_t gPinCount = 0;
Counter gCounters[kMaxCounters];
uint16_t gSampleHz = 50;
int gLedPin = -1;
uint32_t gLastSnapMs = 0;
uint32_t gLastServoMs = 0;
bool gSuspended = false;
String gSuspendJson;

const char *modeName(Mode m) {
  switch (m) {
    case Mode::Input: return "in";
    case Mode::Output: return "out";
    case Mode::Pwm: return "pwm";
    case Mode::Servo: return "servo";
    case Mode::Analog: return "adc";
    default: return "off";
  }
}

const char *modeLabel(Mode m) {
  switch (m) {
    case Mode::Input: return "Input";
    case Mode::Output: return "Output";
    case Mode::Pwm: return "PWM";
    case Mode::Servo: return "Servo";
    case Mode::Analog: return "Analog";
    default: return "Off";
  }
}

bool parseMode(const char *s, Mode &out) {
  if (!s) return false;
  static const struct {
    const char *name;
    Mode mode;
  } table[] = {{"off", Mode::Off},     {"in", Mode::Input},     {"out", Mode::Output},
               {"pwm", Mode::Pwm},     {"servo", Mode::Servo}, {"adc", Mode::Analog}};
  for (const auto &e : table) {
    if (strcmp(s, e.name) == 0) {
      out = e.mode;
      return true;
    }
  }
  return false;
}

const char *pullName(Pull p) {
  switch (p) {
    case Pull::Up: return "up";
    case Pull::Down: return "down";
    default: return "none";
  }
}

bool readNumber(JsonVariantConst v, double &out) {
  if (v.is<bool>()) {
    out = v.as<bool>() ? 1.0 : 0.0;
    return true;
  }
  if (v.is<double>()) {
    out = v.as<double>();
    return isfinite(out);
  }
  return false;
}

template <typename T>
T clampTo(double v, T lo, T hi) {
  if (v < (double)lo) return lo;
  if (v > (double)hi) return hi;
  return (T)lround(v);
}

Pin *findPin(int gpio) {
  for (size_t i = 0; i < gPinCount; i++) {
    if (gPins[i].gpio == gpio) return &gPins[i];
  }
  return nullptr;
}

uint8_t pwmResolution(uint32_t freq) {
  // ESP32-S3 Arduino clocks LEDC from the 40 MHz XTAL (not the 80 MHz APB),
  // so size resolution against 40 MHz or ledc_timer_config() rejects higher
  // frequencies like 25 kHz.
  uint32_t ratio = 40000000UL / freq;
  uint8_t bits = 1;
  while (bits < 14 && (1UL << (bits + 1)) <= ratio) bits++;
  return bits;
}

void writeDuty(Pin &p) {
  uint32_t full = 1UL << p.res;
  uint32_t counts = (uint32_t)lround(p.duty / 100.0 * (double)full);
  if (counts >= full) counts = full - 1;  // keep a low tick at 100% so 4-pin fans still see PWM edges
  ledcWrite(p.gpio, counts);
}

void writeServo(Pin &p) {
  uint32_t counts = (uint32_t)p.usNow * (1UL << kServoBits) / kServoPeriodUs;
  ledcWrite(p.gpio, counts);
}

void applyPull(const Pin &p) {
  gpio_num_t g = (gpio_num_t)p.gpio;
  switch (p.pull) {
    case Pull::Up: gpio_set_pull_mode(g, GPIO_PULLUP_ONLY); break;
    case Pull::Down: gpio_set_pull_mode(g, GPIO_PULLDOWN_ONLY); break;
    default: gpio_set_pull_mode(g, GPIO_FLOATING); break;
  }
}

uint32_t counterRead(const Pin &p) {
  if (p.counter < 0) return 0;
  int v = 0;
  pcnt_unit_get_count(gCounters[p.counter].unit, &v);
  return (uint32_t)v;
}

void resetRate(Pin &p) {
  p.snapHead = 0;
  p.snapCount = 0;
  p.hz = 0.0f;
}

bool counterAttach(Pin &p) {
  int idx = -1;
  for (int i = 0; i < kMaxCounters; i++) {
    if (!gCounters[i].used) {
      idx = i;
      break;
    }
  }
  if (idx < 0) return false;
  Counter &c = gCounters[idx];

  pcnt_unit_config_t ucfg = {};
  ucfg.low_limit = -1;
  ucfg.high_limit = kCounterLimit;
  ucfg.flags.accum_count = 1;
  if (pcnt_new_unit(&ucfg, &c.unit) != ESP_OK) {
    c = Counter{};
    return false;
  }

  pcnt_chan_config_t ccfg = {};
  ccfg.edge_gpio_num = p.gpio;
  ccfg.level_gpio_num = -1;
  if (pcnt_new_channel(c.unit, &ccfg, &c.chan) != ESP_OK) {
    pcnt_del_unit(c.unit);
    c = Counter{};
    return false;
  }
  pcnt_channel_set_edge_action(c.chan, PCNT_CHANNEL_EDGE_ACTION_INCREASE, PCNT_CHANNEL_EDGE_ACTION_HOLD);
  pcnt_channel_set_level_action(c.chan, PCNT_CHANNEL_LEVEL_ACTION_KEEP, PCNT_CHANNEL_LEVEL_ACTION_KEEP);
  pcnt_unit_add_watch_point(c.unit, kCounterLimit);
  pcnt_unit_enable(c.unit);
  pcnt_unit_clear_count(c.unit);
  pcnt_unit_start(c.unit);

  c.used = true;
  p.counter = (int8_t)idx;
  resetRate(p);
  return true;
}

void counterDetach(Pin &p) {
  if (p.counter < 0) return;
  Counter &c = gCounters[p.counter];
  pcnt_unit_stop(c.unit);
  pcnt_unit_disable(c.unit);
  pcnt_unit_remove_watch_point(c.unit, kCounterLimit);
  pcnt_del_channel(c.chan);
  pcnt_del_unit(c.unit);
  c = Counter{};
  p.counter = -1;
  resetRate(p);
}

void counterReset(Pin &p) {
  if (p.counter < 0) return;
  pcnt_unit_clear_count(gCounters[p.counter].unit);
  resetRate(p);
}

void rateSnapshot(Pin &p, uint32_t now) {
  p.snaps[p.snapHead] = {now, counterRead(p)};
  p.snapHead = (uint8_t)((p.snapHead + 1) % kRateWindow);
  if (p.snapCount < kRateWindow) p.snapCount++;
  if (p.snapCount < 2) return;
  uint8_t oldest = (uint8_t)((p.snapHead + kRateWindow - p.snapCount) % kRateWindow);
  uint8_t newest = (uint8_t)((p.snapHead + kRateWindow - 1) % kRateWindow);
  uint32_t dt = p.snaps[newest].ms - p.snaps[oldest].ms;
  uint32_t dc = p.snaps[newest].count - p.snaps[oldest].count;
  p.hz = dt ? (float)dc * 1000.0f / (float)dt : 0.0f;
}

size_t analogCount() {
  size_t n = 0;
  for (size_t i = 0; i < gPinCount; i++) {
    if (gPins[i].mode == Mode::Analog) n++;
  }
  return n;
}

void releaseHardware(Pin &p) {
  if (p.mode == Mode::Input) counterDetach(p);
  if (p.mode == Mode::Pwm || p.mode == Mode::Servo) ledcDetach(p.gpio);
  pinMode(p.gpio, INPUT);
  gpio_set_pull_mode((gpio_num_t)p.gpio, GPIO_FLOATING);
  p.mode = Mode::Off;
  p.sampleCount = 0;
  p.freqActual = 0;
  p.res = 0;
}

void copyConfig(Pin &dst, const Pin &src) {
  dst.pull = src.pull;
  dst.level = src.level;
  dst.freq = src.freq;
  dst.duty = src.duty;
  dst.us = src.us;
  dst.usMin = src.usMin;
  dst.usMax = src.usMax;
  dst.sweep = src.sweep;
  dst.sweepMs = src.sweepMs;
  dst.scale = src.scale;
}

bool setupMode(Pin &p, Mode m, String &err) {
  switch (m) {
    case Mode::Off:
      return true;

    case Mode::Input:
      pinMode(p.gpio, INPUT);
      counterAttach(p);
      applyPull(p);
      return true;

    case Mode::Output:
      digitalWrite(p.gpio, p.level ? HIGH : LOW);
      pinMode(p.gpio, OUTPUT);
      digitalWrite(p.gpio, p.level ? HIGH : LOW);
      return true;

    case Mode::Pwm: {
      uint8_t res = pwmResolution(p.freq);
      if (!ledcAttach(p.gpio, p.freq, res)) {
        err = "No free PWM channel. You can run 8 PWM/servo outputs at up to 4 different frequencies.";
        pinMode(p.gpio, INPUT);
        return false;
      }
      p.res = res;
      p.freqActual = ledcReadFreq(p.gpio);
      writeDuty(p);
      return true;
    }

    case Mode::Servo:
      if (!ledcAttach(p.gpio, kServoHz, kServoBits)) {
        err = "No free PWM channel. You can run 8 PWM/servo outputs at up to 4 different frequencies.";
        pinMode(p.gpio, INPUT);
        return false;
      }
      p.res = kServoBits;
      p.freqActual = kServoHz;
      p.usNow = p.us;
      writeServo(p);
      return true;

    case Mode::Analog:
      pinMode(p.gpio, INPUT);
      (void)analogReadMilliVolts(p.gpio);
      p.sampleCount = 0;
      return true;
  }
  return false;
}

void readParams(JsonObjectConst c, Pin &n) {
  double v;
  const char *pull = c["pull"];
  if (pull) {
    if (strcmp(pull, "up") == 0) n.pull = Pull::Up;
    else if (strcmp(pull, "down") == 0) n.pull = Pull::Down;
    else n.pull = Pull::None;
  }
  if (readNumber(c["v"], v)) n.level = v != 0.0;
  if (readNumber(c["f"], v)) n.freq = clampTo<uint32_t>(v, kPwmFreqMin, kPwmFreqMax);
  if (readNumber(c["du"], v)) n.duty = v < 0 ? 0 : (v > 100 ? 100 : v);
  if (readNumber(c["mn"], v)) n.usMin = clampTo<uint16_t>(v, kServoAbsMin, kServoAbsMax - 100);
  if (readNumber(c["mx"], v)) n.usMax = clampTo<uint16_t>(v, kServoAbsMin + 100, kServoAbsMax);
  if (n.usMax < n.usMin + 100) n.usMax = n.usMin + 100;
  if (readNumber(c["us"], v)) n.us = clampTo<uint16_t>(v, kServoAbsMin, kServoAbsMax);
  if (n.us < n.usMin) n.us = n.usMin;
  if (n.us > n.usMax) n.us = n.usMax;
  if (readNumber(c["sw"], v)) n.sweep = v != 0.0;
  if (readNumber(c["sp"], v)) n.sweepMs = clampTo<uint16_t>(v, 300, 20000);
  if (readNumber(c["sc"], v) && v >= 0.01 && v <= 1000.0) n.scale = v;
}

void pinStateJson(const Pin &p, JsonObject o) {
  o["g"] = p.gpio;
  if (p.owner == Owner::I2C) {
    o["m"] = "i2c";
    return;
  }
  o["m"] = modeName(p.mode);
  switch (p.mode) {
    case Mode::Input:
      o["pull"] = pullName(p.pull);
      o["cnt"] = p.counter >= 0;
      break;
    case Mode::Output:
      o["v"] = p.level ? 1 : 0;
      break;
    case Mode::Pwm:
      o["f"] = p.freq;
      o["fa"] = p.freqActual;
      o["du"] = round(p.duty * 10.0) / 10.0;
      o["r"] = p.res;
      break;
    case Mode::Servo:
      o["us"] = p.us;
      o["mn"] = p.usMin;
      o["mx"] = p.usMax;
      o["sw"] = p.sweep;
      o["sp"] = p.sweepMs;
      break;
    case Mode::Analog:
      o["sc"] = p.scale;
      break;
    default:
      break;
  }
}

}  // namespace

void pinsBegin(int statusLedPin) {
  gLedPin = statusLedPin;
  gPinCount = 0;
  for (uint8_t g : kCandidatePins) {
    if ((int)g == gLedPin) continue;
    Pin &p = gPins[gPinCount++];
    p = Pin{};
    p.gpio = g;
    p.adc = g >= 1 && g <= 10;
    pinMode(g, INPUT);
    gpio_set_pull_mode((gpio_num_t)g, GPIO_FLOATING);
  }
  logf("[pins] %u bench pins ready", (unsigned)gPinCount);
}

bool pinsCommand(JsonObjectConst cmd, String &err) {
  int gpio = cmd["g"] | -1;
  Pin *p = findPin(gpio);
  if (!p) {
    err = "GPIO " + String(gpio) + " isn't available on the bench.";
    return false;
  }
  if (p->owner != Owner::User) {
    err = "GPIO " + String(gpio) + " belongs to the I2C bus right now. Release the bus on the I2C tab first.";
    return false;
  }

  Mode target = p->mode;
  const char *m = cmd["m"];
  if (m && !parseMode(m, target)) {
    err = String("Unknown pin mode \"") + m + "\".";
    return false;
  }

  Pin next = *p;
  readParams(cmd, next);

  bool modeChange = target != p->mode;
  if (target == Mode::Analog && !p->adc) {
    err = "Only GPIO 1-10 can measure voltage (they're on ADC1, which keeps working with WiFi on).";
    return false;
  }
  if (target == Mode::Analog && modeChange && analogCount() >= kMaxAnalog) {
    err = "The scope has 4 channels. Set another analog pin to Off first.";
    return false;
  }

  bool rebuild = modeChange;
  if (rebuild) {
    Pin prev = *p;
    Mode prevMode = p->mode;
    releaseHardware(*p);
    copyConfig(*p, next);
    if (!setupMode(*p, target, err)) {
      releaseHardware(*p);
      copyConfig(*p, prev);
      String ignore;
      if (prevMode != Mode::Off && setupMode(*p, prevMode, ignore)) p->mode = prevMode;
      return false;
    }
    p->mode = target;
    if (target == Mode::Input && p->counter < 0) {
      logf("[pins] GPIO %u input without counter (all 4 pulse counters busy)", p->gpio);
    }
    return true;
  }

  double resetFlag = 0.0;
  switch (target) {
    case Mode::Input:
      if (next.pull != p->pull) {
        p->pull = next.pull;
        applyPull(*p);
      }
      if (readNumber(cmd["reset"], resetFlag) && resetFlag != 0.0) counterReset(*p);
      break;
    case Mode::Output:
      p->level = next.level;
      digitalWrite(p->gpio, p->level ? HIGH : LOW);
      break;
    case Mode::Pwm:
      if (next.freq != p->freq) {
        uint8_t res = pwmResolution(next.freq);
        if (ledcChangeFrequency(p->gpio, next.freq, res) == 0) {
          err = "Couldn't set that PWM frequency on this pin.";
          return false;
        }
        p->freq = next.freq;
        p->res = res;
        p->freqActual = ledcReadFreq(p->gpio);
      }
      p->duty = next.duty;
      writeDuty(*p);
      break;
    case Mode::Servo:
      p->us = next.us;
      p->usMin = next.usMin;
      p->usMax = next.usMax;
      p->sweepMs = next.sweepMs;
      p->sweep = next.sweep;
      if (!p->sweep) {
        p->usNow = p->us;
        writeServo(*p);
      }
      break;
    case Mode::Analog:
      p->scale = next.scale;
      break;
    default:
      copyConfig(*p, next);
      break;
  }
  return true;
}

namespace {
void deenergizeAll() {
  for (size_t i = 0; i < gPinCount; i++) {
    Pin &p = gPins[i];
    if (p.owner == Owner::User && p.mode != Mode::Off) releaseHardware(p);
  }
}
}  // namespace

void pinsAllOff() {
  deenergizeAll();
  gSuspended = false;
  gSuspendJson = "";
}

void pinsConfigJson(JsonObject out) {
  JsonArray arr = out["pins"].to<JsonArray>();
  for (size_t i = 0; i < gPinCount; i++) {
    const Pin &p = gPins[i];
    if (p.owner != Owner::User || p.mode == Mode::Off) continue;
    pinStateJson(p, arr.add<JsonObject>());
  }
  out["rate"] = gSampleHz;
}

bool pinsApplyConfig(JsonObjectConst cfg, String &err) {
  deenergizeAll();
  gSuspended = false;
  gSuspendJson = "";
  int failures = 0;
  String firstErr;
  for (JsonObjectConst e : cfg["pins"].as<JsonArrayConst>()) {
    String e2;
    if (!pinsCommand(e, e2)) {
      failures++;
      if (firstErr.isEmpty()) firstErr = e2;
    }
  }
  int hz = cfg["rate"] | 0;
  if (hz > 0) pinsSetSampleHz((uint16_t)hz);
  if (failures) {
    err = firstErr;
    return false;
  }
  return true;
}

void pinsSuspend() {
  if (gSuspended) return;
  JsonDocument doc;
  pinsConfigJson(doc.to<JsonObject>());
  gSuspendJson = "";
  serializeJson(doc, gSuspendJson);
  gSuspended = true;
  deenergizeAll();
}

bool pinsResume(String &err) {
  if (!gSuspended) {
    err = "Nothing is suspended.";
    return false;
  }
  JsonDocument doc;
  if (deserializeJson(doc, gSuspendJson)) {
    gSuspended = false;
    gSuspendJson = "";
    err = "The suspended setup was lost.";
    return false;
  }
  String e2;
  pinsApplyConfig(doc.as<JsonObjectConst>(), e2);
  gSuspended = false;
  gSuspendJson = "";
  return true;
}

bool pinsSuspended() { return gSuspended; }

void pinsCancelSuspend() {
  gSuspended = false;
  gSuspendJson = "";
}

bool pinsHasActiveOutputs() {
  for (size_t i = 0; i < gPinCount; i++) {
    Mode m = gPins[i].mode;
    if (m == Mode::Output || m == Mode::Pwm || m == Mode::Servo) return true;
  }
  return false;
}

void pinsLoop(uint32_t nowMs) {
  if (nowMs - gLastSnapMs >= 100) {
    gLastSnapMs = nowMs;
    for (size_t i = 0; i < gPinCount; i++) {
      Pin &p = gPins[i];
      if (p.mode == Mode::Input && p.counter >= 0) rateSnapshot(p, nowMs);
    }
  }
  if (nowMs - gLastServoMs >= 20) {
    gLastServoMs = nowMs;
    for (size_t i = 0; i < gPinCount; i++) {
      Pin &p = gPins[i];
      if (p.mode != Mode::Servo || !p.sweep) continue;
      uint32_t period = p.sweepMs < 300 ? 300 : p.sweepMs;
      float phase = (float)(nowMs % period) / (float)period;
      float tri = phase < 0.5f ? phase * 2.0f : 2.0f - phase * 2.0f;
      p.usNow = (uint16_t)(p.usMin + lroundf(tri * (float)(p.usMax - p.usMin)));
      writeServo(p);
    }
  }
}

void pinsSample() {
  for (size_t i = 0; i < gPinCount; i++) {
    Pin &p = gPins[i];
    if (p.mode != Mode::Analog) continue;
    uint32_t sum = 0;
    for (uint8_t k = 0; k < kAdcOversample; k++) sum += analogReadMilliVolts(p.gpio);
    uint16_t mv = (uint16_t)(sum / kAdcOversample);
    if (p.sampleCount >= kSampleBuf) {
      memmove(p.samples, p.samples + 1, (kSampleBuf - 1) * sizeof(p.samples[0]));
      p.sampleCount = kSampleBuf - 1;
    }
    p.samples[p.sampleCount++] = mv;
  }
}

uint16_t pinsSampleHz() { return gSampleHz; }

bool pinsSetSampleHz(uint16_t hz) {
  for (uint16_t r : kRates) {
    if (r == hz) {
      gSampleHz = hz;
      return true;
    }
  }
  return false;
}

bool pinsHasAnalog() { return analogCount() > 0; }

bool pinsIsUserPin(uint8_t gpio) { return findPin(gpio) != nullptr; }

bool pinsClaim(uint8_t gpio, Owner owner, String &err) {
  Pin *p = findPin(gpio);
  if (!p) {
    err = "GPIO " + String(gpio) + " isn't available on the bench.";
    return false;
  }
  if (p->owner == owner) return true;
  if (p->owner != Owner::User) {
    err = "GPIO " + String(gpio) + " is already claimed.";
    return false;
  }
  if (p->mode != Mode::Off) {
    err = "GPIO " + String(gpio) + " is set to " + modeLabel(p->mode) + ". Set it to Off first.";
    return false;
  }
  p->owner = owner;
  return true;
}

void pinsRelease(uint8_t gpio, Owner owner) {
  Pin *p = findPin(gpio);
  if (!p || p->owner != owner) return;
  p->owner = Owner::User;
  p->mode = Mode::Off;
  pinMode(gpio, INPUT);
  gpio_set_pull_mode((gpio_num_t)gpio, GPIO_FLOATING);
}

void pinsCatalogJson(JsonObject out) {
  JsonArray pins = out["pins"].to<JsonArray>();
  for (size_t i = 0; i < gPinCount; i++) {
    JsonObject o = pins.add<JsonObject>();
    o["g"] = gPins[i].gpio;
    if (gPins[i].adc) o["adc"] = true;
  }
  JsonObject lim = out["limits"].to<JsonObject>();
  lim["analog"] = kMaxAnalog;
  lim["pwm"] = 8;
  lim["pwmTimers"] = 4;
  lim["counters"] = kMaxCounters;
  lim["fMin"] = kPwmFreqMin;
  lim["fMax"] = kPwmFreqMax;
  lim["usMin"] = kServoAbsMin;
  lim["usMax"] = kServoAbsMax;
  JsonArray rates = out["rates"].to<JsonArray>();
  for (uint16_t r : kRates) rates.add(r);
  out["statusLed"] = gLedPin;
}

void pinsStateJson(JsonArray out) {
  for (size_t i = 0; i < gPinCount; i++) {
    const Pin &p = gPins[i];
    if (p.mode == Mode::Off && p.owner == Owner::User) continue;
    pinStateJson(p, out.add<JsonObject>());
  }
}

void pinsLiveJson(JsonObject out) {
  JsonObject a = out["a"].to<JsonObject>();
  JsonObject d = out["d"].to<JsonObject>();
  JsonObject c = out["c"].to<JsonObject>();
  JsonObject s = out["s"].to<JsonObject>();
  for (size_t i = 0; i < gPinCount; i++) {
    Pin &p = gPins[i];
    if (p.owner != Owner::User) continue;
    String key(p.gpio);
    switch (p.mode) {
      case Mode::Analog: {
        JsonArray arr = a[key].to<JsonArray>();
        for (uint8_t k = 0; k < p.sampleCount; k++) arr.add(p.samples[k]);
        p.sampleCount = 0;
        break;
      }
      case Mode::Input: {
        d[key] = digitalRead(p.gpio) ? 1 : 0;
        if (p.counter >= 0) {
          JsonArray arr = c[key].to<JsonArray>();
          arr.add(counterRead(p));
          arr.add(round((double)p.hz * 10.0) / 10.0);
        }
        break;
      }
      case Mode::Output:
        d[key] = p.level ? 1 : 0;
        break;
      case Mode::Servo:
        s[key] = p.usNow;
        break;
      default:
        break;
    }
  }
}

}  // namespace bb
