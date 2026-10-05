# BenchBuddy v1.0.1

A wireless test bench for your ESP32-S3. Plug a sensor, LED, fan, servo or I2C module into the board, open a web page on your phone or PC, and poke at it: flip pins, run PWM, sweep servos, graph voltages, count pulses, and scan the I2C bus.

The board hosts the page itself. No app, no cloud, no internet needed.

- **Board:** ESP32-S3 DevKitC-1 style, **N16R8** module (16 MB flash, 8 MB PSRAM), RGB LED on GPIO38
- **Base:** works with the "ESP32_S3 GPIO Extension Board" (DC jack, 3-pin rows). See [Using the extension board](#using-the-extension-board).
- **Built with:** PlatformIO, Arduino core 3.3.12 (pioarduino 55.03.312)
- **Tested:** compiles with zero warnings; every screen tested in 3 themes at desktop and phone sizes; the pin engine was run against a 57-step test script

---

## What's in v1

| Tab | What it does |
|---|---|
| **Pins** | A map of the board. Tap a pin and set it to Off, Input, Output, PWM, Servo or Analog. Reserved pins explain why they're off-limits. |
| **Scope** | Live voltage graph, up to 4 channels, 10–200 samples per second, with min/max/average and CSV export. Input and output pins show as logic traces underneath. |
| **I2C** | Bus scanner with a classic address map, wiring health check (pull-ups, stuck lines, auto-recovery), a "probably this chip" guess for 50+ common modules across 48 addresses, and a register read/write tool. |
| **System** | WiFi setup, firmware updates over WiFi, failsafe switch, device info, and the theme picker. |

**Three themes** (switch anytime, top right or System → Appearance):

- **Lab Instrument:** dark slate with scope-style channel colors
- **Clean Studio:** bright and calm for daylight benches
- **Cyberdeck:** amber terminal glow with sharp corners

The red **All off** button is on every screen and turns every output off instantly.

---

## Folder layout

```
BenchBuddy/
├─ platformio.ini        Build settings (board, memory, libraries)
├─ include/config.h      Settings you might change (names, hotspot password, LED pin)
├─ src/                  Firmware
│  ├─ main.cpp           Main loop and command handling
│  ├─ pins.cpp           Pin engine: input/output/PWM/servo/analog/pulse counters
│  ├─ i2c_tools.cpp      I2C scan, bus health check, register read/write
│  ├─ net.cpp            WiFi, hotspot fallback, captive portal, mDNS
│  ├─ web.cpp            Web server, WebSocket, firmware updates
│  ├─ status_led.cpp     Onboard RGB LED colors
│  └─ web_assets.h       The web page, packed automatically from web/ (don't edit)
├─ web/index.html        The whole web interface (edit this one)
├─ web/fonts/            The 10 fonts the themes use (open-source, OFL)
├─ scripts/embed_web.py  Packs web/ into src/web_assets.h on every build
├─ firmware/             Ready-to-flash builds (no compiling needed)
└─ docs/PROTOCOL.md      Every message the page and board send each other
```

---

## Flash it

You have two options. Option A is fastest. Option B is what you'll use once you start changing code.

### Option A: flash the ready-made file (no compiling)

1. Plug the board into your PC with a data USB cable.
2. Open the Spacehuhn ESP Web Tool (`esp.huhn.me`) in Chrome or Edge, click **Connect** and pick the board's COM port.
3. Choose `firmware/BenchBuddy-v1.0.1-factory.bin` and set the address to **0x0**.
4. Click **Program**. When it finishes, press the board's **RST** button.

Or from a terminal with esptool:

```
esptool --chip esp32s3 write-flash 0x0 firmware/BenchBuddy-v1.0.1-factory.bin
```

> **Factory vs OTA file:** the `factory.bin` holds everything (bootloader, partition table, app) and goes to address 0x0 over USB. The `ota.bin` is only the app. Use that one in **System → Firmware update** once BenchBuddy is already running.

### Option B: build and upload with PlatformIO

1. Open VS Code with the PlatformIO extension (you already have `.platformio` set up).
2. **File → Open Folder** and pick this `BenchBuddy` folder.
3. Plug in the board and click the **→ Upload** arrow in the bottom bar. The first build downloads the ESP32 toolchain (a few hundred MB), so give it a few minutes.
4. Click the **plug icon (Serial Monitor)**. You'll see the board start up and print its address.

**If the upload won't start:** hold **BOOT**, tap **RST**, let go of **BOOT**, then upload again. That forces the chip into flash mode.

**If the build fails with path errors on Windows:** the build folders get deep. Keep the project at a short path like `C:\BenchBuddy`, or turn on Windows long paths (Settings → System → For developers → Enable long paths).

**Which USB port?** Boards with two USB-C ports label them **USB** (the chip's own USB) and **UART/COM** (a USB-serial chip). Either one works for uploading and for the serial log. BenchBuddy prints to both.

---

## First start

1. **Join the hotspot.** On first boot there's no WiFi saved, so BenchBuddy makes its own network: **`BenchBuddy-XXXX`**, password **`benchbuddy`**. The onboard LED glows **purple**.
2. **Open the page.** Most phones pop it open automatically (it acts like hotel WiFi sign-in). If not, go to **http://192.168.4.1**.
3. **Add your WiFi.** System tab → **WiFi setup** → **Scan** → tap your network → type the password → **Save and restart**.
4. **Reconnect.** Put your phone or PC back on your home WiFi and open **http://benchbuddy.local**. The LED turns **green**.

If `benchbuddy.local` doesn't load (some Android phones don't support `.local` names), use the IP address instead. It's printed in the serial monitor and shown in your router's device list.

**If your WiFi is down,** BenchBuddy keeps trying for 30 seconds, then brings its hotspot back so you're never locked out. While in hotspot mode it retries your WiFi every 2 minutes whenever nobody's connected to the hotspot.

### Status LED

| LED | Meaning |
|---|---|
| Blinking blue | Connecting to WiFi |
| Green | On your WiFi |
| Purple | Hotspot mode |
| Blinking white | Firmware update in progress |
| Blinking red | Failsafe turned the outputs off |

The LED is on **GPIO38** to match your board's pinout (`RGB_LED`). If it stays dark after flashing, your board uses GPIO48: change it under **System → RGB status LED**. No reflashing needed.

---

## Using the tabs

### Pins

Tap a pin on the board map. The panel shows what that pin can do, then pick a mode:

- **Off:** high-impedance. The pin isn't driving anything. This is the safe default and what every pin starts at.
- **Input:** reads HIGH/LOW live. Choose a **pull resistor**: pull-up (reads HIGH until something pulls it to ground, good for buttons wired to GND) or pull-down. The **pulse counter** counts edges in hardware and shows frequency and RPM, so it can read a PC fan's tach wire (2 pulses per revolution).
- **Output:** HIGH (3.3 V) or LOW (0 V).
- **PWM:** fast on/off switching. **Duty** is the share of each cycle it's on (40% duty ≈ 40% power). Presets cover LED dimming (5 kHz), buzzers (2 kHz) and 4-pin PC fans (25 kHz).
- **Servo:** sends the 50 Hz pulse hobby servos expect. Drag the pulse width, jump to min/center/max, or turn on **Sweep**.
- **Analog:** measures voltage (GPIO 1–10 only). Pick a **voltage divider** preset if your signal is above 3.3 V and the reading is scaled back up for you.

**Quick starts** in the empty panel set up common tests in one tap: LED, potentiometer, button, PC fan, servo sweep.

### Scope

Every Analog pin becomes a channel (CH1–CH4). Pick a time window (5–60 s), auto or full-range scaling, and the sample rate. Hover or touch the graph to read exact values. **CSV** saves the visible window.

### I2C

1. Pick SDA and SCL (defaults are GPIO 8 and 9), keep 100 kHz to start, and tap **Scan bus**.
2. **Bus health** tells you if pull-up resistors are present and if either line is stuck low. If SDA is stuck, BenchBuddy clocks SCL to free it.
3. Found devices light up in the address map with a best guess at what they are. **Read ID** reads the chip's ID register and tells you what value to expect (for example an MPU-6050 at 0x68 answers `68` from register 0x75).
4. The **Register tool** reads or writes any register. For writes, the register goes first, then your bytes.

While the bus is on, its two pins are locked. **Release pins** frees them.

### System

- **Failsafe:** when on, every output turns off if no browser has been connected for 10 seconds. Turn it on for motors and heaters.
- **Firmware update:** pick `firmware.bin` (or `firmware/BenchBuddy-…-ota.bin`) and flash it over WiFi. Outputs switch off during the update, and if the upload breaks, the board keeps its old firmware.
- **RGB status LED:** which pin drives the onboard LED (GPIO 38, GPIO 48 or none). The board restarts to apply it, and the unused pin becomes a bench pin.
- **Restart** and **Forget WiFi** ask you to tap twice so you can't hit them by accident.

---

## Wiring rules (read once, saves boards)

1. **3.3 V max on any GPIO.** These pins are not 5 V tolerant. 5 V on a pin can kill it.
2. **About 20 mA per pin.** Enough for an LED with a 220–330 Ω resistor. Motors, relays, LED strips and solenoids need a transistor or MOSFET between them and the pin.
3. **Always share ground.** If your device has its own power supply, connect its GND to the board's GND or readings will be garbage.
4. **Servos:** signal wire to the GPIO, red to **5V** (not 3V3), brown/black to GND. On the extension board that means *not* the middle pin of the 3-pin rows (see below).
5. **4-pin PC fan:** black = GND, yellow = 12 V from a separate supply, green = tach → an Input pin with pull-up, blue = PWM → a PWM pin at 25 kHz. Never connect the fan's 12 V to the board.

### Measuring more than 3.3 V

Use a voltage divider: **signal → R1 → pin → R2 → GND**. The pin sees `V × R2 / (R1 + R2)`. Tell BenchBuddy which divider you used and it scales the reading back up.

| R1 (top) | R2 (to GND) | Multiplier | Safe up to about |
|---|---|---|---|
| none | none | ×1 | 3.1 V |
| 10 kΩ | 10 kΩ | ×2 | 6 V |
| 30 kΩ | 7.5 kΩ | ×5 | 15 V (the blue "0–25 V sensor" module) |
| 100 kΩ | 22 kΩ | ×5.55 | 17 V |

The ESP32's ADC is good for "is it about right" readings (within a few percent), not lab-grade measurements.

---

## Which pins you can use

**Bench pins (24):** 1, 2, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 21, 39, 40, 41, 42, 47, 48

**Voltage measurement:** GPIO 1–10 only (ADC1). The other ADC (ADC2, GPIO 11–20) shuts off whenever WiFi is on.

**Locked out on purpose:**

| Pins | Why |
|---|---|
| 0, 3, 45, 46 | Strapping pins. Their level at power-up decides how the chip boots. |
| 19, 20 | Native USB data lines. |
| 43, 44 | UART0 TX/RX, the serial log. |
| 35, 36, 37 | Wired to the 8 MB PSRAM inside the N16R8 module. |
| 38 | Onboard RGB status LED. If you switch the LED to GPIO 48 in System, 38 becomes a bench pin and 48 gets locked instead. |

**Limits:** 4 analog channels, 8 PWM/servo outputs at up to 4 different frequencies (all servos share one), 4 hardware pulse counters, PWM from 5 Hz to 100 kHz.

---

## Using the extension board

Your "ESP32_S3 GPIO Extension Board" breaks every pin out into 3-pin rows plus separate power headers.

- **3-pin rows are GND / 3.3V / S.** S is the GPIO signal. The middle pin is **3.3 V**, so it's right for sensors and modules, but **not** for servos, fans or anything with a motor.
- **Servos:** signal to the row's **S** pin, servo power from the **5V output pins** block, ground from any GND.
- **I2C modules:** SDA to the S pin of IO8, SCL to the S pin of IO9, and VCC/GND from the 3.3V output block or the rows.
- **DC jack (6.5–9 V)** feeds the board through a 78M05 regulator, which is rated about 500 mA and runs warm at 9 V. Good for the ESP32, sensors and a small servo. Give big servos, motors and fans their own supply and share ground.
- The labels on the rows (IO4, IO5…) match the GPIO numbers in BenchBuddy, so "GPIO 7" on the Pins tab is the row marked IO7.

---

## Settings you might change

Open `include/config.h`:

| Setting | Default | Change it when |
|---|---|---|
| `BB_DEFAULT_HOSTNAME` | `benchbuddy` | You run two bench units (also changeable on the WiFi screen) |
| `BB_AP_PASSWORD` | `benchbuddy` | Always a good idea. 8+ characters. |
| `BB_DEFAULT_WIFI_SSID` / `_PASS` | empty | You want to skip the WiFi setup screen |
| `BB_STATUS_LED_PIN` | `38` | Only the first-boot default. Easier to change under System → RGB status LED. |
| `BB_I2C_DEFAULT_SDA` / `_SCL` | `8` / `9` | Your modules are wired elsewhere |

Rebuild and upload after changing these.

---

## Security note

BenchBuddy has no login. Anyone on the same WiFi can open it, switch pins and flash firmware. That's fine on your home network, but don't port-forward it to the internet, and change the hotspot password in `config.h`. A PIN lock is on the v2 list.

---

## Troubleshooting

| Problem | Fix |
|---|---|
| Page won't load at `benchbuddy.local` | Use the IP from the serial monitor or router. Check your phone is on the same WiFi. |
| "Reconnecting" in the top bar | The board rebooted, lost WiFi, or your phone slept. It reconnects by itself. |
| Analog reads ~0 or jumps around | Nothing's connected (a floating pin wanders), or ground isn't shared. |
| Input flickers HIGH/LOW | Turn on a pull resistor. |
| I2C finds nothing | Check 3V3 and GND to the module, swap SDA/SCL, try 100 kHz, look at Bus health. |
| "No free PWM channel" | You've hit 8 outputs or 4 different frequencies. Turn one off or reuse a frequency. |
| Board resets when a motor starts | Brownout: the motor pulls the supply down. Power motors separately and share ground. Last reset shows "Brownout" on the System tab. |

---

## What's next

- **v2:** serial bridge (talk to GPS modules or another ESP32 through the page), SPI chip-ID checks for the nRF24L01+ and CC1101, PIN lock
- **v3:** INA219/INA226 current monitoring with auto power cutoff, logic analyzer with PulseView export

To add a feature, `docs/PROTOCOL.md` lists every message the page and board exchange.

---

## Credits

- Libraries: ESPAsyncWebServer and AsyncTCP (ESP32Async), ArduinoJson (Benoît Blanchon)
- Fonts (SIL Open Font License, see `web/fonts/FONT_LICENSES.txt`): Barlow Semi Condensed, IBM Plex Mono, Instrument Sans, JetBrains Mono, Share Tech Mono, VT323
