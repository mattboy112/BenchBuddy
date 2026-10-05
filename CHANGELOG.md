# Changelog

## v1.1.0 (2026-10-04)

**New**
- **Suspend / Resume.** One tap (top bar, or System → Setups & configs) pauses every output but remembers the whole setup. Resume puts every pin back the way it was. A banner shows while the bench is suspended.
- **Setups & configs** on the System tab. Save up to 6 named setups on the board (they survive a power-cycle), load them back in one tap, delete old ones.
- **Share setups as files.** Export the current setup or a saved one as a `.json` file, and import someone else's file to apply it (optionally saving it under a name).

**Fixed (rolled in from v1.0.2 – v1.0.4)**
- PWM at higher frequencies like 25 kHz (4-pin PC fans) now works. The PWM resolution is sized for the ESP32-S3's 40 MHz LEDC clock instead of 80 MHz, which made the driver reject those frequencies.
- 100% duty keeps a tiny low tick, so 4-pin fans still see PWM edges at full duty.
- Changing a PWM pin's frequency now retunes it in place instead of tearing the pin down and rebuilding it.

> v1.0.2, v1.0.3 and v1.0.4 were quick test builds of the PWM fixes above. Exactly which fix landed in which of those three wasn't recorded, so they're listed together.

## v1.0.1 (2026-10-04)

First tracked release.

- Pins tab: any bench pin as Off, Input (with pulse counter / RPM), Output, PWM (5 Hz – 100 kHz), Servo (with sweep) or Analog (with voltage-divider presets)
- Scope tab: live voltage graph, up to 4 channels, 10–200 samples/s, CSV export
- I2C tab: bus scanner, bus health check (pull-ups, stuck lines, auto-recovery), chip guesses for 50+ common modules, register read/write
- System tab: WiFi setup, firmware updates over WiFi, failsafe, device info, theme picker, status LED pin (GPIO38 / GPIO48 / none)
- Joins your WiFi or makes its own `BenchBuddy-XXXX` hotspot, captive portal, `benchbuddy.local`
- Three themes: Lab Instrument, Clean Studio, Cyberdeck
- Red **All off** button on every screen
