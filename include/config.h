#pragma once

#define BB_FW_VERSION "1.0.1"

// Name on your network: http://benchbuddy.local
#define BB_DEFAULT_HOSTNAME "benchbuddy"

// Hotspot used on first boot, or whenever home WiFi can't be reached.
// Network name becomes BenchBuddy-XXXX (last 4 of the MAC address).
#define BB_AP_SSID_PREFIX "BenchBuddy-"
#define BB_AP_PASSWORD "benchbuddy"

// Optional: fill these in to skip the WiFi setup screen on first boot.
// Anything saved from the web page overrides them.
#define BB_DEFAULT_WIFI_SSID ""
#define BB_DEFAULT_WIFI_PASS ""

#define BB_STA_CONNECT_TIMEOUT_MS 15000UL
#define BB_STA_LOST_TO_AP_MS 30000UL
#define BB_STA_RETRY_INTERVAL_MS 120000UL

// Onboard RGB LED. N16R8 DevKitC-1 boards marked "RGB_LED" on GPIO38 use 38,
// older v1.0 boards use 48, -1 means no LED. That pin is hidden from the bench.
// This is only the first-boot default: switch it on the System tab without reflashing.
#define BB_STATUS_LED_PIN 38
#define BB_STATUS_LED_LEVEL 18

#define BB_I2C_DEFAULT_SDA 8
#define BB_I2C_DEFAULT_SCL 9

// Failsafe: outputs switch off when no browser has been connected this long
#define BB_FAILSAFE_TIMEOUT_MS 10000UL
