# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

ESP32-S3 weather station serving wind speed/direction and BME280 environmental data (temperature, humidity, pressure) over Modbus TCP. Designed to be read by the ESP32APRS_Audio_HARC client project. Wind data is currently test data with auto-incrementing values; BME280 readings are real.

## Build Commands

This is a PlatformIO project using the Arduino framework on the pioarduino ESP32 platform fork.

```bash
# Build
pio run

# Upload to board
pio run --target upload

# Serial monitor (115200 baud, USB CDC)
pio device monitor

# Build + upload + monitor
pio run --target upload && pio device monitor

# Clean build
pio run --target clean
```

## Architecture

**Single-file application:** All code lives in `src/main.cpp`.

- **WiFi connection:** Connects to hardcoded SSID/password (overridable via `MY_SSID`/`MY_PASS` defines)
- **ModbusServerTCPasync:** Async Modbus TCP server on port 502, server ID 1, 1 client max, 20s timeout
- **`data[REG_COUNT]`:** Holding register storage served by FC 0x03 worker (currently 5 registers)
- **Test auto-increment:** Register 0 increments by 5 per read (+0.5 km/hr), register 1 increments by 5 degrees (wraps at 360). BME280 registers are real readings, not test data.
- **BME280:** Polled every 5 seconds in `loop()` via `readBME280()`. I2C on SDA=GPIO6, SCL=GPIO5. Tries address 0x76 then 0x77. If sensor is missing, registers 2/3/4 stay at 0 and the rest of the server still runs. Latest readings are cached as floats (`lastTempC`, `lastHumidity`, `lastPressureHpa`) so the HTTP debug page can show raw sensor values without an extra I2C transaction.
- **HTTP debug server:** Synchronous `WebServer` on port 80. `GET /` returns a self-refreshing HTML page (5s) showing the raw sensor floats, the encoded register values, and what the Modbus client decodes them to — useful for diagnosing the whole pipeline from a browser when the device is deployed somewhere without serial console access. Served from the main loop via `http.handleClient()`.

### Register Map

| Register | Contents | Scaling | Initial Value | Test Increment |
|----------|----------|---------|---------------|----------------|
| 0 | Wind speed | ×10 (raw 103 = 10.3 km/hr) | 103 | +5 per read (+0.5 km/hr) |
| 1 | Wind direction | degrees (0–359) | 0 | +5 per read (wraps at 360) |
| 2 | Temperature (BME280) | offset-encoded: raw = °C × 10 + 1000 (e.g. 1235 = 23.5°C, 950 = -5.0°C) | 0 | none — real sensor |
| 3 | Humidity (BME280) | ×10 %RH (e.g. 587 = 58.7%) | 0 | none — real sensor |
| 4 | Pressure (BME280) | ×10 hPa (e.g. 10133 = 1013.3 hPa) | 0 | none — real sensor |

Note on temperature: stored unsigned with a +1000 offset so the register supports negatives without needing signed-int16 client decoding. Decode formula is `°C = raw × 0.1 − 100`, expressed in the client as equation `b=0.1, c=-100`. Representable range is -100.0°C (raw 0) to +5453.5°C (raw 65535) — well beyond anything the BME280 itself can read (-40 to +85°C). The firmware clamps to 0..65535 just in case.

### Key Libraries
- **eModbus** (v1.7.4) — Modbus TCP/RTU library. Pulled from GitHub via `lib_deps`. Transitively brings in AsyncTCP and Ethernet.
- **Adafruit BME280 Library** (^2.2.4) — I2C/SPI driver for the BME280 temperature/humidity/pressure sensor. Brings in Adafruit Unified Sensor as a dependency.

## ESP32APRS_Audio_HARC Client Configuration

The companion client project at `/Users/rick/Sync/Projects/PlatformIO/ESP32APRS_Audio_HARC` uses eModbus TCP to read from this server.

### Global Modbus Settings (client web UI or config)

| Setting | Value |
|---------|-------|
| Enable | `true` |
| Channel | `4` (TCP) |
| TCP Host | IP address printed on this server's serial console at boot |
| TCP Port | `502` |

### Sensor Slot Settings

The client encodes address as `slave_addr * 1000 + register_addr`. Only FC 0x03 is used. Use `MODBUS_16Bit` (port 24) — **not** `MODBUS_32Bit`.

#### Wind Speed

| Setting | Value |
|---------|-------|
| Port | `MODBUS_16Bit` (24) |
| Address | `1000` (server 1, register 0) |
| Equation a | `0` |
| Equation b | `0.1` |
| Equation c | `0` |
| WX Mapping | WindSpd |

Raw value 103 × 0.1 = 10.3 km/hr.

#### Wind Direction

| Setting | Value |
|---------|-------|
| Port | `MODBUS_16Bit` (24) |
| Address | `1001` (server 1, register 1) |
| Equation a | `0` |
| Equation b | `1` |
| Equation c | `0` |
| WX Mapping | WindDir |

Raw value is degrees (0–359), no scaling needed.

#### Temperature (BME280)

| Setting | Value |
|---------|-------|
| Port | `MODBUS_16Bit` (24) |
| Address | `1002` (server 1, register 2) |
| Equation a | `0` |
| Equation b | `0.1` |
| Equation c | `-100` |
| WX Mapping | Temp |

Offset encoding: raw 1235 → 1235 × 0.1 − 100 = 23.5 °C; raw 950 → 950 × 0.1 − 100 = -5.0 °C. The `c=-100` term is what cancels the +1000 offset added on the server side.

#### Humidity (BME280)

| Setting | Value |
|---------|-------|
| Port | `MODBUS_16Bit` (24) |
| Address | `1003` (server 1, register 3) |
| Equation a | `0` |
| Equation b | `0.1` |
| Equation c | `0` |
| WX Mapping | Humidity |

Raw value 587 × 0.1 = 58.7 %RH.

#### Pressure (BME280)

| Setting | Value |
|---------|-------|
| Port | `MODBUS_16Bit` (24) |
| Address | `1004` (server 1, register 4) |
| Equation a | `0` |
| Equation b | `0.1` |
| Equation c | `0` |
| WX Mapping | Pressure |

Raw value 10133 × 0.1 = 1013.3 hPa.

### Known Issue: knots vs mph

The APRS weather packet wind speed field is supposed to be mph, but some receivers interpret it as knots. The client currently converts kph to knots (`× 0.5399` in `weather.cpp`) as a workaround. The standard conversion would be `× 0.621` for mph.

## Hardware Configuration

- **Board:** `waveshare_esp32_s3_zero` (ESP32-S3, native USB)
- **USB:** CDC on boot enabled (`ARDUINO_USB_MODE=1`, `ARDUINO_USB_CDC_ON_BOOT=1`) — Serial output goes through native USB, no external UART
- **Platform:** pioarduino fork of platform-espressif32 (stable release)
- **ESP-IDF requirement:** >= 5.1 (specified in `src/idf_component.yml`)
- **BME280 wiring:** I2C — SDA on GPIO6, SCL on GPIO5, 3.3V, GND. Default I2C address 0x76 (most modules); 0x77 also auto-detected.

## Important Notes

- WiFi credentials are hardcoded in `src/main.cpp` — use `MY_SSID`/`MY_PASS` build defines to override
