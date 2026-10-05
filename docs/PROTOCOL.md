# BenchBuddy protocol

How the web page and the board talk. Use this when adding features or writing your own client (a Python script, another ESP32, a home server dashboard).

Two channels:

- **WebSocket** at `ws://<board>/ws` for everything live: pin control, streaming readings, I2C.
- **HTTP** under `/api/` for one-off jobs: device info, WiFi setup, restart, firmware upload.

All WebSocket messages are JSON objects with a `t` (type) field. Commands may include an `id`. If a command fails, the `err` reply carries the same `id`.

---

## Page → board (WebSocket)

| `t` | Fields | What it does |
|---|---|---|
| `pin` | `g` GPIO, plus any of the fields below | Change a pin's mode and/or settings |
| `alloff` | none | Every pin back to Off (I2C bus stays on) |
| `rate` | `hz`: 10, 20, 50, 100 or 200 | Analog sample rate |
| `failsafe` | `on`: true/false | Turn off outputs when no browser is connected for 10 s (saved across restarts) |
| `led` | `pin`: 38, 48 or -1 | Which pin drives the RGB status LED. Saved, then the board restarts. |
| `i2c_cfg` | `sda`, `scl`, `hz`: 10000, 50000, 100000 or 400000 | Start or move the I2C bus (claims both pins) |
| `i2c_off` | none | Stop the bus and free its pins |
| `i2c_scan` | none | Bus health check plus scan of 0x08–0x77. Starts the bus if needed. |
| `i2c_read` | `a` address, `r` register (-1 for none), `n` bytes 1–32 | Read bytes |
| `i2c_write` | `a` address, `b` array of 1–32 bytes | Write bytes (put the register first) |
| `ping` | none | Keep-alive. The page sends one every 3 s. |

### `pin` fields

| Field | Used by | Meaning |
|---|---|---|
| `m` | all | Mode: `off`, `in`, `out`, `pwm`, `servo`, `adc` |
| `pull` | in | `none`, `up`, `down` |
| `reset` | in | `1` clears the pulse counter |
| `v` | out | `1` HIGH, `0` LOW |
| `f` | pwm | Frequency in Hz, 5–100000 |
| `du` | pwm | Duty in percent, 0–100 |
| `us` | servo | Pulse width in µs, clamped to `mn`–`mx` |
| `mn`, `mx` | servo | Pulse limits, 400–2600 µs, at least 100 µs apart |
| `sw` | servo | `1` to sweep between `mn` and `mx` |
| `sp` | servo | Sweep period in ms, 300–20000 |
| `sc` | adc | Divider multiplier, 0.01–1000 |

Out-of-range numbers are clamped, not rejected. Examples:

```json
{"t":"pin","g":7,"m":"pwm","f":25000,"du":40,"id":12}
{"t":"pin","g":7,"du":65,"id":13}
{"t":"pin","g":4,"m":"adc","sc":5,"id":14}
{"t":"pin","g":15,"m":"servo","us":1500,"sw":1,"sp":2000,"id":15}
```

---

## Board → page (WebSocket)

On connect the board sends `hello`, `state` and `sys`, in that order.

### `hello` (also `GET /api/info`)

```json
{"t":"hello","fw":"1.0.0","build":"Oct  4 2026 01:27:10","chip":"ESP32-S3","rev":2,"cores":2,"mhz":240,
 "flash":16777216,"psram":8388608,"sketch":1328528,"sketchMax":6553600,"reset":"Power on",
 "pins":[{"g":1,"adc":true},{"g":11}],
 "limits":{"analog":4,"pwm":8,"pwmTimers":4,"counters":4,"fMin":5,"fMax":100000,"usMin":400,"usMax":2600},
 "rates":[10,20,50,100,200],"statusLed":38,
 "net":{"mode":"wifi","host":"benchbuddy","apSsid":"BenchBuddy-7F3A","saved":"HomeNet","mac":"…","ssid":"HomeNet","ip":"192.168.1.87","rssi":-52,"ch":6}}
```

`rev` is major×100 + minor (2 = v0.2). `net.mode` is `wifi`, `hotspot` or `connecting`.

### `state` (after every change, to every browser)

Only pins that aren't Off are listed.

```json
{"t":"state","rate":50,"fs":false,"i2c":{"on":true,"sda":8,"scl":9,"hz":100000},
 "pins":[
   {"g":4,"m":"adc","sc":1},
   {"g":6,"m":"in","pull":"up","cnt":true},
   {"g":7,"m":"pwm","f":25000,"fa":25000,"du":40,"r":11},
   {"g":8,"m":"i2c"},
   {"g":15,"m":"servo","us":1500,"mn":500,"mx":2500,"sw":false,"sp":2000},
   {"g":16,"m":"out","v":1}]}
```

`fa` is the frequency the hardware actually produces and `r` the duty resolution in bits. `cnt` is false when all 4 pulse counters are taken.

### `live` (every 50 ms while a browser is connected)

```json
{"t":"live","up":3723050,"hz":50,
 "a":{"4":[1912,1915,1921]},
 "d":{"6":1,"16":1},
 "c":{"6":[18342,80.0]},
 "s":{"15":1500}}
```

- `a`: raw pin millivolts for each analog pin, oldest first, one every `1000/hz` ms ending at `up`. Multiply by `sc` for the real voltage.
- `d`: current level of input and output pins.
- `c`: pulse counter `[total rising edges, Hz over the last second]`.
- `s`: current servo pulse in µs (moves while sweeping).

### `sys` (every 2 s)

```json
{"t":"sys","up":3724000,"heap":281000,"heapMin":251344,"heapSize":368640,"psramFree":8247300,"temp":41.3,"clients":1,"net":{…}}
```

### Replies

```json
{"t":"err","msg":"The scope has 4 channels. Set another analog pin to Off first.","id":14}
{"t":"notice","msg":"Failsafe switched every output off because no browser was connected for 10 seconds."}
{"t":"i2c_scan","sda":8,"scl":9,"hz":100000,"bus":{"pullSda":true,"pullScl":true,"sda":true,"scl":true},"found":[60,104,118],"ok":true,"ms":38}
{"t":"i2c_rw","op":"read","a":104,"r":117,"n":1,"ok":true,"data":[104]}
{"t":"i2c_rw","op":"write","a":104,"n":2,"ok":false,"err":"No ACK. Nothing answered at that address."}
```

In `bus`, `pullSda`/`pullScl` mean external pull-up resistors were detected, `sda`/`scl` mean the line idles high, and `recovered: true` means a stuck SDA line was freed by clocking SCL.

---

## HTTP

| Method and path | Body | Reply |
|---|---|---|
| `GET /` | | The page (gzipped) |
| `GET /api/info` | | Same as `hello` |
| `GET /api/wifi/scan?fresh=1` | | Starts a scan. Poll without `fresh` until `state` is `done` or `error`. |
| `POST /api/wifi` | form: `ssid`, `pass`, `host` | Saves WiFi and restarts |
| `POST /api/wifi/forget` | | Clears WiFi and restarts into hotspot mode |
| `POST /api/reboot` | | Restarts |
| `POST /api/update` | multipart file upload | Flashes the uploaded app `.bin`, then restarts |

Scan reply:

```json
{"state":"done","nets":[{"ssid":"HomeNet","rssi":-51,"open":false,"ch":6}]}
```

In hotspot mode, any unknown host or path redirects to `http://192.168.4.1/` so phones open the page automatically.

---

## Quick test from a PC

Python, with `pip install websockets`:

```python
import asyncio, json, websockets

async def main():
    async with websockets.connect("ws://benchbuddy.local/ws") as ws:
        await ws.send(json.dumps({"t": "pin", "g": 16, "m": "out", "v": 1}))
        for _ in range(5):
            print(await ws.recv())

asyncio.run(main())
```
