// ==========================================================================================================
// This project is frmo an emodbus example
//
// It implements a Modbus TCP server on an ESP32-S3 that serves BME280 sensor data in holding registers.
// The server listens on port 502 and supports a single client connection. It also runs an
// HTTP server on port 80 that serves a self-refreshing status page showing the raw sensor values,
// the encoded register values, and the values the Modbus client will decode, so the whole
// pipeline can be checked at a glance.
//
// Wiring:
//   BME280 SDA → GPIO6
//   BME280 SCL → GPIO5
//   BME280 GND → GND
//   BME280 VCC → 3.3V 
//
// Note: the BME280 has a default I2C address of 0x76, but some modules use 0x77. The code
// tries both addresses on startup. If the sensor is not found, the registers will stay at 0
// and the status page will show a warning.
//
// This project also simulates a wind speed and direction which can be used with sensor that
// can provide this data.
//
// Modbus register map:
//   Register 0: Wind speed (scaled x10, e.g. 103 = 10.3 km/hr) — test data that auto-increments on each read
//   Register 1: Wind direction (degrees, 0-359) — test data that auto
//                 increments by 5 degrees on each read, wraps at 360
//   Register 2: Temperature (offset-encoded: raw = °C * 10
//                 + 1000, e.g. 1235 = 23.5°C, 950 = -5.0°C)
//                 Decode: °C = raw * 0.1 - 100   (client
//                 eqn b=0.1, c=-100)
//                 Range: -100.0°C (raw 0) .. +5453.5°C (raw 65535)  
//   Register 3: Humidity (scaled x10, e.g. 587 = 58.7 %RH)
//   Register 4: Pressure (scaled x10 hPa, e.g. 101
//                 33 = 1013.3 hPa)
//
// The code uses the Adafruit BME280 library for sensor interfacing and the AsyncTCP library for handling
// TCP connections.      
// =============================================================================================================

#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <Wire.h>
#include <Adafruit_Sensor.h>
#include <Adafruit_BME280.h>
#include "ModbusServerTCPasync.h"

// Connect to wifi via the access point (AP) in ESP32APRS_Audio. It should give us
// an IP address of something like 192.168.4.2 and will print it to the serial
// console on commection.
// Another option is to connect both devices to a router but we will still
// need to know this boards IP address because it needs to be used to setup 
// a modbus connection.

#ifndef MY_SSID
#define MY_SSID "ESP32APRS"
#endif
#ifndef MY_PASS
#define MY_PASS "ESP32APRS"
#endif

char ssid[] = MY_SSID;
char pass[] = MY_PASS;

ModbusServerTCPasync MBserver;

// HTTP debug server on port 80 — serves a self-refreshing sensor status page
WebServer http(80);

// BME280 I2C pins (waveshare ESP32-S3 zero)
#define SDAPIN 6
#define SCLPIN 5

Adafruit_BME280 bme;
bool bmeFound = false;

// Latest BME280 readings cached as floats so the HTTP handler can show
// the raw sensor values alongside the encoded register values without
// triggering an extra I2C transaction per request.
float lastTempC = NAN;
float lastHumidity = NAN;
float lastPressureHpa = NAN;
unsigned long lastReadMillis = 0;

// Holding registers
// Register 0: Wind speed (scaled x10, e.g. 103 = 10.3 km/hr)
// Register 1: Wind direction (degrees, 0-359)
// Register 2: Temperature (offset-encoded: raw = °C * 10 + 1000, e.g. 1235 = 23.5°C, 950 = -5.0°C)
//             Decode: °C = raw * 0.1 - 100   (client eqn b=0.1, c=-100)
//             Range: -100.0°C (raw 0) .. +5453.5°C (raw 65535)
// Register 3: Humidity (scaled x10, e.g. 587 = 58.7 %RH)
// Register 4: Pressure (scaled x10 hPa, e.g. 10133 = 1013.3 hPa)
#define REG_COUNT 5
#define TEMP_OFFSET 1000  // raw = °C * 10 + TEMP_OFFSET → keeps register unsigned
uint16_t data[REG_COUNT];

// Worker function for FC 0x03 (Read Holding Registers)
// After each read, test registers for wind speed and directionauto-increment for testing:
//   Register 0: +5 per read (+0.5 km/hr with client b=0.1)
//   Register 1: +5 per read (+5 degrees, wraps at 360)
ModbusMessage FC03(ModbusMessage request)
{
    ModbusMessage response;
    uint16_t addr = 0;
    uint16_t words = 0;
    request.get(2, addr);
    request.get(4, words);

    if((addr + words) > REG_COUNT)
    {
        response.setError(request.getServerID(), request.getFunctionCode(), ILLEGAL_DATA_ADDRESS);
        return response;
    }

    response.add(request.getServerID(), request.getFunctionCode(), (uint8_t)(words * 2));

    for(uint8_t i = 0; i < words; ++i)
    {
        response.add(data[addr + i]);
        uint16_t reg = addr + i;
        if(reg == 0)
        {
            data[reg] += 5;  // Wind speed: +0.5 km/hr per read
        }
        else if(reg == 1)
        {
            data[reg] = (data[reg] + 5) % 360;  // Wind direction: +5 degrees, wraps at 360
        }
    }

    return response;
}

// Read BME280 and update holding registers 2/3/4
void readBME280()
{
    if(!bmeFound)
    {
        return;
    }

    float t = bme.readTemperature();         // °C
    float h = bme.readHumidity();            // %RH
    float p = bme.readPressure() / 100.0f;   // hPa

    // Cache for the HTTP debug endpoint
    lastTempC = t;
    lastHumidity = h;
    lastPressureHpa = p;
    lastReadMillis = millis();

    // Temperature: offset-encoded so the register stays unsigned and supports negatives.
    // raw = °C * 10 + 1000  →  client decodes as raw * 0.1 - 100
    long tEncoded = lroundf(t * 10.0f) + TEMP_OFFSET;
    if(tEncoded < 0)
    {
        tEncoded = 0;
    }
    else if(tEncoded > 65535)
    {
        tEncoded = 65535;
    }
    data[2] = (uint16_t)tEncoded;
    data[3] = (uint16_t)lroundf(h * 10.0f);
    data[4] = (uint16_t)lroundf(p * 10.0f);

    Serial.printf("BME280: T=%.1f C  H=%.1f %%  P=%.1f hPa\n", t, h, p);
}

// Render the debug status page. Shows raw sensor floats alongside the
// encoded register values and the values the Modbus client will decode,
// so the whole pipeline can be checked at a glance.
void handleRoot()
{
    String html;
    html.reserve(2048);
    html += F("<!DOCTYPE html><html><head>");
    html += F("<meta http-equiv=\"refresh\" content=\"5\">");
    html += F("<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">");
    html += F("<title>WX-Modbus Status</title>");
    html += F("<style>body{font-family:monospace;padding:1em;max-width:780px;}");
    html += F("table{border-collapse:collapse;width:100%;margin-bottom:1em;}");
    html += F("th,td{padding:6px 10px;border-bottom:1px solid #ccc;text-align:left;}");
    html += F("th{background:#eee;}h2{margin-top:1.5em;}.muted{color:#888;}</style>");
    html += F("</head><body>");
    html += F("<h1>ESP32-S3-WX-Modbus</h1>");
    html += F("<p class=\"muted\">Auto-refreshes every 5 seconds.</p>");

    html += F("<h2>BME280</h2>");
    if(!bmeFound)
    {
        html += F("<p><b>Sensor not found.</b> Check wiring (SDA=GPIO6, SCL=GPIO5) and I2C address.</p>");
    }
    else
    {
        unsigned long ageMs = millis() - lastReadMillis;
        html += F("<p class=\"muted\">Last read ");
        html += String(ageMs / 1000);
        html += F(" s ago.</p>");

        html += F("<table><tr><th>Reading</th><th>Sensor (raw float)</th><th>Register</th><th>Client decodes as</th></tr>");

        // Temperature
        html += F("<tr><td>Temperature</td><td>");
        html += String(lastTempC, 2);
        html += F(" &deg;C</td><td>");
        html += String(data[2]);
        html += F("</td><td>");
        html += String((float)data[2] * 0.1f - 100.0f, 1);
        html += F(" &deg;C</td></tr>");

        // Humidity
        html += F("<tr><td>Humidity</td><td>");
        html += String(lastHumidity, 2);
        html += F(" %RH</td><td>");
        html += String(data[3]);
        html += F("</td><td>");
        html += String((float)data[3] * 0.1f, 1);
        html += F(" %RH</td></tr>");

        // Pressure
        html += F("<tr><td>Pressure</td><td>");
        html += String(lastPressureHpa, 2);
        html += F(" hPa</td><td>");
        html += String(data[4]);
        html += F("</td><td>");
        html += String((float)data[4] * 0.1f, 1);
        html += F(" hPa</td></tr>");

        html += F("</table>");
    }

    html += F("<h2>Wind (test data)</h2>");
    html += F("<table><tr><th>Reading</th><th>Register</th><th>Client decodes as</th></tr>");
    html += F("<tr><td>Wind speed</td><td>");
    html += String(data[0]);
    html += F("</td><td>");
    html += String((float)data[0] * 0.1f, 1);
    html += F(" km/hr</td></tr>");
    html += F("<tr><td>Wind direction</td><td>");
    html += String(data[1]);
    html += F("</td><td>");
    html += String(data[1]);
    html += F(" &deg;</td></tr>");
    html += F("</table>");

    html += F("<h2>System</h2>");
    html += F("<table>");
    html += F("<tr><td>IP</td><td>");
    html += WiFi.localIP().toString();
    html += F("</td></tr>");
    html += F("<tr><td>Uptime</td><td>");
    html += String(millis() / 1000);
    html += F(" s</td></tr>");
    html += F("<tr><td>Free heap</td><td>");
    html += String(ESP.getFreeHeap());
    html += F(" bytes</td></tr>");
    html += F("</table>");

    html += F("</body></html>");

    http.send(200, "text/html", html);
}

void setup()
{
    Serial.begin(115200);
    delay(2000);

    // I2C bus for BME280
    Wire.begin(SDAPIN, SCLPIN);

#if 0 // set to 1 to create an access point
    IPAddress local_IP(192, 168, 4, 10);
    IPAddress gateway(192, 168, 4, 1);
    IPAddress subnet(255, 255, 255, 0);
    IPAddress primaryDNS(8, 8, 8, 8);

    WiFi.config(local_IP, gateway, subnet, primaryDNS);
#endif
    // Connect to WiFi
    WiFi.begin(ssid, pass);
    delay(200);
    while(WiFi.status() != WL_CONNECTED)
    {
        Serial.print(". ");
        delay(500);
    }
    IPAddress wIP = WiFi.localIP();
    Serial.printf("WiFi IP address: %u.%u.%u.%u\n", wIP[0], wIP[1], wIP[2], wIP[3]);

    // Initialize BME280 — try common I2C addresses
    if(bme.begin(0x76, &Wire))
    {
        bmeFound = true;
        Serial.println("BME280 found at 0x76");
    }
    else if(bme.begin(0x77, &Wire))
    {
        bmeFound = true;
        Serial.println("BME280 found at 0x77");
    }
    else
    {
        Serial.println("BME280 not found - temp/humidity/pressure registers will stay 0");
    }

    // Initialize register values
    data[0] = 103;  // Wind speed: 10.3 km/hr (test data)
    data[1] = 0;    // Wind direction: 0 degrees (test data)
    data[2] = 0;    // Temperature
    data[3] = 0;    // Humidity
    data[4] = 0;    // Pressure

    // Take an initial sensor reading so registers are populated before first poll
    readBME280();

    // Register worker and start on port 502
    MBserver.registerWorker(1, READ_HOLD_REGISTER, &FC03);
    MBserver.start(502, 1, 20000);

    // HTTP debug server on port 80
    http.on("/", handleRoot);
    http.begin();
    Serial.printf("HTTP debug page: http://%u.%u.%u.%u/\n", wIP[0], wIP[1], wIP[2], wIP[3]);
}

void loop()
{
    http.handleClient();

    static unsigned long lastSensorMillis = 0;
    if(millis() - lastSensorMillis > 5000)
    {
        lastSensorMillis = millis();
        readBME280();
    }
}

