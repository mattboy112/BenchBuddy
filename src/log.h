#pragma once

#include <Arduino.h>

namespace bb {

void logBegin();
void logf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

}  // namespace bb
