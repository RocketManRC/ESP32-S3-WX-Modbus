# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

ESP32-S3 weather station serving wind speed and direction over Modbus TCP. Designed to be read by the ESP32APRS_Audio_HARC client project. Currently uses test data with auto-incrementing values.

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
- **`data[2]`:** Holding register storage served by FC 0x03 worker
- **Test auto-increment:** Register 0 increments by 5 per read (+0.5 km/hr), register 1 increments by 5 degrees (wraps at 360)
- **Loop:** Prints free heap every 10 seconds

### Register Map

| Register | Contents | Scaling | Initial Value | Test Increment |
|----------|----------|---------|---------------|----------------|
| 0 | Wind speed | ×10 (raw 103 = 10.3 km/hr) | 103 | +5 per read (+0.5 km/hr) |
| 1 | Wind direction | degrees (0–359) | 0 | +5 per read (wraps at 360) |

### Key Library
- **eModbus** (v1.7.4) — Modbus TCP/RTU library. Pulled from GitHub via `lib_deps`. Transitively brings in AsyncTCP and Ethernet.

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

### Known Issue: knots vs mph

The APRS weather packet wind speed field is supposed to be mph, but some receivers interpret it as knots. The client currently converts kph to knots (`× 0.5399` in `weather.cpp`) as a workaround. The standard conversion would be `× 0.621` for mph.

## Hardware Configuration

- **Board:** `waveshare_esp32_s3_zero` (ESP32-S3, native USB)
- **USB:** CDC on boot enabled (`ARDUINO_USB_MODE=1`, `ARDUINO_USB_CDC_ON_BOOT=1`) — Serial output goes through native USB, no external UART
- **Platform:** pioarduino fork of platform-espressif32 (stable release)
- **ESP-IDF requirement:** >= 5.1 (specified in `src/idf_component.yml`)

## Important Notes

- WiFi credentials are hardcoded in `src/main.cpp` — use `MY_SSID`/`MY_PASS` build defines to override
- Not currently a git repository
