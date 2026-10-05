#include "i2c_tools.h"

#include <Wire.h>

#include "config.h"
#include "log.h"
#include "pins.h"

namespace bb {
namespace {

bool gOn = false;
uint8_t gSda = BB_I2C_DEFAULT_SDA;
uint8_t gScl = BB_I2C_DEFAULT_SCL;
uint32_t gHz = 100000;

constexpr uint32_t kSpeeds[] = {10000, 50000, 100000, 400000};
constexpr uint8_t kMaxRead = 32;

const char *errText(uint8_t code) {
  switch (code) {
    case 2: return "No ACK. Nothing answered at that address.";
    case 3: return "The device stopped acknowledging data partway through.";
    case 5: return "Timed out. The bus may be stuck low or missing pull-up resistors.";
    default: return "Bus error. Check wiring and power.";
  }
}

bool validSpeed(uint32_t hz) {
  for (uint32_t s : kSpeeds) {
    if (s == hz) return true;
  }
  return false;
}

void busCheck(JsonObject bus) {
  pinMode(gSda, INPUT_PULLDOWN);
  pinMode(gScl, INPUT_PULLDOWN);
  delayMicroseconds(300);
  bool extSda = digitalRead(gSda);
  bool extScl = digitalRead(gScl);

  pinMode(gSda, INPUT_PULLUP);
  pinMode(gScl, INPUT_PULLUP);
  delayMicroseconds(300);
  bool sdaHigh = digitalRead(gSda);
  bool sclHigh = digitalRead(gScl);

  bool recovered = false;
  if (!sdaHigh && sclHigh) {
    pinMode(gScl, OUTPUT_OPEN_DRAIN | PULLUP);
    for (int i = 0; i < 9 && !digitalRead(gSda); i++) {
      digitalWrite(gScl, LOW);
      delayMicroseconds(10);
      digitalWrite(gScl, HIGH);
      delayMicroseconds(10);
    }
    pinMode(gSda, OUTPUT_OPEN_DRAIN | PULLUP);
    digitalWrite(gSda, LOW);
    delayMicroseconds(10);
    digitalWrite(gScl, HIGH);
    delayMicroseconds(10);
    digitalWrite(gSda, HIGH);
    delayMicroseconds(10);
    pinMode(gSda, INPUT_PULLUP);
    pinMode(gScl, INPUT_PULLUP);
    delayMicroseconds(100);
    sdaHigh = digitalRead(gSda);
    sclHigh = digitalRead(gScl);
    recovered = sdaHigh;
  }

  bus["pullSda"] = extSda;
  bus["pullScl"] = extScl;
  bus["sda"] = sdaHigh;
  bus["scl"] = sclHigh;
  if (recovered) bus["recovered"] = true;
}

bool ensureOn(JsonObject out) {
  if (gOn) return true;
  String err;
  if (i2cConfigure(gSda, gScl, gHz, err)) return true;
  out["ok"] = false;
  out["err"] = err;
  return false;
}

}  // namespace

bool i2cConfigure(int sda, int scl, uint32_t hz, String &err) {
  if (!validSpeed(hz)) {
    err = "Pick 10, 50, 100 or 400 kHz.";
    return false;
  }
  if (sda == scl) {
    err = "SDA and SCL need to be different pins.";
    return false;
  }
  if (sda < 0 || scl < 0 || !pinsIsUserPin((uint8_t)sda) || !pinsIsUserPin((uint8_t)scl)) {
    err = "Pick SDA and SCL from the bench pins.";
    return false;
  }

  if (gOn && sda == gSda && scl == gScl) {
    if (hz != gHz) {
      Wire.setClock(hz);
      gHz = hz;
    }
    return true;
  }

  bool sdaWasOurs = gOn && (sda == gSda || sda == gScl);
  if (!pinsClaim((uint8_t)sda, Owner::I2C, err)) return false;
  if (!pinsClaim((uint8_t)scl, Owner::I2C, err)) {
    if (!sdaWasOurs) pinsRelease((uint8_t)sda, Owner::I2C);
    return false;
  }

  if (gOn) {
    Wire.end();
    if (gSda != sda && gSda != scl) pinsRelease(gSda, Owner::I2C);
    if (gScl != sda && gScl != scl) pinsRelease(gScl, Owner::I2C);
  }

  gSda = (uint8_t)sda;
  gScl = (uint8_t)scl;
  gHz = hz;
  if (!Wire.begin(gSda, gScl, gHz)) {
    pinsRelease(gSda, Owner::I2C);
    pinsRelease(gScl, Owner::I2C);
    gOn = false;
    err = "Couldn't start I2C on those pins.";
    return false;
  }
  Wire.setTimeOut(50);
  gOn = true;
  logf("[i2c] bus on SDA=%u SCL=%u @ %lu Hz", gSda, gScl, (unsigned long)gHz);
  return true;
}

void i2cRelease() {
  if (!gOn) return;
  Wire.end();
  pinsRelease(gSda, Owner::I2C);
  pinsRelease(gScl, Owner::I2C);
  gOn = false;
  logf("[i2c] bus released");
}

void i2cStateJson(JsonObject out) {
  out["on"] = gOn;
  out["sda"] = gSda;
  out["scl"] = gScl;
  out["hz"] = gHz;
}

void i2cScan(JsonObject out) {
  out["sda"] = gSda;
  out["scl"] = gScl;
  out["hz"] = gHz;
  if (!ensureOn(out)) return;

  uint32_t t0 = millis();
  Wire.end();
  busCheck(out["bus"].to<JsonObject>());
  if (!Wire.begin(gSda, gScl, gHz)) {
    out["ok"] = false;
    out["err"] = "Couldn't restart I2C on those pins.";
    return;
  }

  Wire.setTimeOut(25);
  JsonArray found = out["found"].to<JsonArray>();
  uint8_t failures = 0;
  bool aborted = false;
  for (uint8_t addr = 0x08; addr <= 0x77; addr++) {
    Wire.beginTransmission(addr);
    uint8_t e = Wire.endTransmission();
    if (e == 0) {
      found.add(addr);
    } else if (e == 4 || e == 5) {
      if (++failures >= 3) {
        aborted = true;
        break;
      }
    }
  }
  Wire.setTimeOut(50);

  out["ok"] = !aborted;
  if (aborted) out["err"] = "Scan stopped because the bus keeps timing out. Check wiring, power and pull-up resistors.";
  out["ms"] = millis() - t0;
  logf("[i2c] scan: %u device(s) in %lu ms", (unsigned)found.size(), (unsigned long)(millis() - t0));
}

void i2cRead(uint8_t addr, int reg, uint8_t len, JsonObject out) {
  if (len < 1) len = 1;
  if (len > kMaxRead) len = kMaxRead;
  out["op"] = "read";
  out["a"] = addr;
  out["r"] = reg;
  out["n"] = len;
  if (!ensureOn(out)) return;

  if (reg >= 0) {
    Wire.beginTransmission(addr);
    Wire.write((uint8_t)reg);
    uint8_t e = Wire.endTransmission(false);
    if (e != 0) {
      out["ok"] = false;
      out["err"] = errText(e);
      return;
    }
  }
  size_t got = Wire.requestFrom(addr, (size_t)len);
  if (got == 0) {
    out["ok"] = false;
    out["err"] = reg >= 0 ? "Read failed. The device didn't answer that register." : errText(2);
    return;
  }
  JsonArray data = out["data"].to<JsonArray>();
  while (Wire.available()) data.add(Wire.read());
  out["ok"] = true;
}

void i2cWrite(uint8_t addr, const uint8_t *data, size_t len, JsonObject out) {
  out["op"] = "write";
  out["a"] = addr;
  out["n"] = len;
  if (!ensureOn(out)) return;
  Wire.beginTransmission(addr);
  Wire.write(data, len);
  uint8_t e = Wire.endTransmission();
  out["ok"] = e == 0;
  if (e != 0) out["err"] = errText(e);
}

}  // namespace bb
