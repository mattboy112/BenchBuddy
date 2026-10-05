#include "log.h"

#include <stdarg.h>

namespace bb {

void logBegin() {
  Serial.begin(115200);
#if ARDUINO_USB_CDC_ON_BOOT && ARDUINO_USB_MODE
  Serial.setTxTimeoutMs(0);
  Serial0.begin(115200);
#endif
}

void logf(const char *fmt, ...) {
  char buf[256];
  va_list args;
  va_start(args, fmt);
  int n = vsnprintf(buf, sizeof(buf) - 2, fmt, args);
  va_end(args);
  if (n < 0) return;
  size_t len = (size_t)n < sizeof(buf) - 2 ? (size_t)n : sizeof(buf) - 3;
  buf[len++] = '\r';
  buf[len++] = '\n';
  buf[len] = '\0';
  Serial.write(reinterpret_cast<const uint8_t *>(buf), len);
#if ARDUINO_USB_CDC_ON_BOOT && ARDUINO_USB_MODE
  Serial0.write(reinterpret_cast<const uint8_t *>(buf), len);
#endif
}

}  // namespace bb
