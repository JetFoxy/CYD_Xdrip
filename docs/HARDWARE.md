# Hardware Notes — CYD (ESP32-2432S028)

## Board Overview

**ESP32-2432S028** ("Cheap Yellow Display")

- MCU: ESP32 (dual-core 240 MHz)
- Display: ILI9341 2.8" TFT 320×240, SPI
- Touch: XPT2046 resistive (not used in this firmware)
- Backlight: GPIO 21, active HIGH, PWM via LEDC channel 0
- SD card slot: VSPI (GPIO 18/19/23), CS = GPIO 5
- USB: micro-USB, CH340 UART bridge

---

## Connectors

### CN1 — Speaker

2-pin JST 1.25mm.
Connected to GPIO 26 via on-board mono amplifier.

**Required speaker:** 8Ω 0.5W, any size (typically 28–36mm diameter).
Connector: JST 1.25mm 2-pin (or solder directly).

```
CN1 Pin 1 — Speaker +
CN1 Pin 2 — Speaker −
```

Used in firmware for BG alarms (LEDC channel 1, GPIO 26).

---

### P3 — GPIO Expansion

4-pin JST 1.25mm.

```
P3 Pin 1 — GND
P3 Pin 2 — 3.3V
P3 Pin 3 — GPIO 22  →  Alarm reset button
P3 Pin 4 — GPIO 27  →  Brightness cycle button
```

**Button wiring:** connect between GPIO pin and GND.
Internal pull-up enabled in firmware (`INPUT_PULLUP`) — no external resistors needed.

**Recommended buttons:** 6×6×5mm tactile switch (momentary, 4-pin DIP).
For panel mounting: M12 momentary push button with threaded collar.

---

### BOOT Button (GPIO 0)

On-board button. Also cycles brightness — same function as P3 brightness button.

---

## SD Card — CYD.INI Configuration File

Place `CYD.INI` in the root of the SD card (FAT32 formatted).

```ini
[config]
; Units: 0 = mmol/L, 1 = mg/dL (also overridden by BLE packet)
show_mgdl=0

; UTC offset in minutes. UTC+3 (Moscow) = 180, UTC+5 = 300
; Used as fallback when BLE app does not send timezone.
; When WiFi/NTP is active, NTP takes priority.
utc_offset_min=180

; BG alert thresholds in mmol/L (or mg/dL if show_mgdl=1)
bg_low=3.9
bg_warn_low=4.5
bg_warn_high=9.0
bg_high=10.0

; Initial brightness level: 0 = dim, 1 = medium, 2 = bright
brightness=2

; BLE auth password. If omitted, device auto-generates one.
; blepassword=mypassword

[wifi]
; Leave ssid empty to disable WiFi entirely.
ssid=MyNetwork
password=MyPassword

[nightscout]
; Full URL without trailing slash.
url=https://mysite.herokuapp.com
; Access token (optional, depends on Nightscout auth settings).
; token=mytoken-xxxxxxxxxxxxxxxx
```

---

## GPIO Summary

| GPIO | Function | Notes |
|------|----------|-------|
| 0    | BOOT button (brightness cycle) | Active LOW, internal pull-up |
| 5    | SD card CS | VSPI |
| 18   | SD card SCK | VSPI |
| 19   | SD card MISO | VSPI |
| 21   | TFT backlight | PWM, LEDC ch 0 |
| 22   | P3 — alarm reset button | Active LOW, internal pull-up |
| 23   | SD card MOSI | VSPI |
| 26   | CN1 speaker | PWM, LEDC ch 1 |
| 27   | P3 — brightness button | Active LOW, internal pull-up |

---

## Alarm Behaviour

| Condition | Alarm type | Sound |
|-----------|-----------|-------|
| BG < `bg_low` or BG > `bg_high` | Urgent | 3 × short high beep (1800 Hz) |
| BG < `bg_warn_low` or BG > `bg_warn_high` | Warning | 1 × medium beep (900 Hz) |

- Repeats every 5 minutes while condition persists.
- Press **alarm reset button** (P3 GPIO 22) to silence.
- Silence clears automatically when BG returns to normal range.

---

## BLE Protocol (WatchDrip CYD)

Device name: `M5Stack` (for compatibility with WatchDrip Android app).

- Service UUID: `AF6E5F78-706A-43FB-B1F4-C27D7D5C762F`
- Characteristic UUID: `6D810E9F-0983-4030-BDA7-C7C9A6A19C1C`
- Properties: WRITE_NR + NOTIFY

### Opcodes received by device

**0x09 — Auth init**
- Device has no password → generates random 10-char password, sends it via 0x0E
- Device has password → sends 0x0F (bypass)

**0x0A — Auth pass**
- Payload: password bytes
- Device replies 0x0B (success) or 0x0C (fail)

**0x20 — WatchDrip update** (main data packet)

| Bytes | Content |
|-------|---------|
| 0 | Opcode 0x20 |
| 1–2 | Packet number / total |
| 3–4 | BG value, mg/dL, uint16 big-endian |
| 5–6 | Reserved |
| 7–10 | UTC timestamp, uint32 big-endian |
| 11 | Trend direction (0–7, see below) |
| 12 | Flags: bit0=mg/dL, bit1=stale |
| 13–14 | UTC offset, int16 big-endian, minutes |
| 15–16 | IoB × 100, uint16 big-endian |
| 17–18 | Last bolus × 100, uint16 big-endian |
| 19 | Last bolus age, minutes |
| 20–21 | Pump IoB × 100, uint16 big-endian |
| 22–23 | Pump reservoir × 10, uint16 big-endian |
| 24 | Pump battery % (255 = no pump) |
| 25 | Carbs on board, grams |

Trend directions: 0=NONE, 1=DoubleUp, 2=SingleUp, 3=FortyFiveUp, 4=Flat, 5=FortyFiveDown, 6=SingleDown, 7=DoubleDown

**0x21 — BG history pre-fill**

| Bytes | Content |
|-------|---------|
| 0 | Opcode 0x21 |
| 1 | Count (max 36) |
| 2 | Reserved |
| 3…3+count×2 | BG values mg/dL, uint16 big-endian, newest first |

### Opcodes sent by device

| Opcode | Meaning |
|--------|---------|
| 0x0B | Auth success |
| 0x0C | Auth failed |
| 0x0E | New generated password (payload: password string) |
| 0x0F | No password required |

---

## Power

Powered via micro-USB (5V). On-board 3.3V LDO supplies ESP32 and peripherals.
P3 pin 2 provides 3.3V for external sensors/buttons (max ~200 mA shared).
