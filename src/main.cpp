// =================================================================================================
// This project is frmo an emodbus example
// =================================================================================================

#include <Arduino.h>
#include <WiFi.h>
#include "ModbusServerTCPasync.h"

// Connect to wifi via the access point (AP) in ESP32APRS_Audio. It should give us
// an IP address of something like 192.168.4.2 and will print it to the serial
// console on commection.
// Another option is to connect both devices to a router but we will still
// need to know this boards IP address because it needs to be used to setup 
// a modbus connection.
#ifndef MY_SSID
#define MY_SSID "ESP32APRS_Audio"
#endif
#ifndef MY_PASS
#define MY_PASS "aprsthnetwork"
#endif

char ssid[] = MY_SSID;
char pass[] = MY_PASS;

ModbusServerTCPasync MBserver;

// Holding registers
// Register 0: Wind speed (scaled x10, e.g. 103 = 10.3 km/hr)
// Register 1: Wind direction (degrees, 0-359)
uint16_t data[2];

// Worker function for FC 0x03 (Read Holding Registers)
// After each read, test registers auto-increment for testing:
//   Register 0: +5 per read (+0.5 km/hr with client b=0.1)
//   Register 1: +5 per read (+5 degrees, wraps at 360)
ModbusMessage FC03(ModbusMessage request)
{
    ModbusMessage response;
    uint16_t addr = 0;
    uint16_t words = 0;
    request.get(2, addr);
    request.get(4, words);

    if((addr + words) > 2)
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

void setup()
{
    Serial.begin(115200);
    delay(2000);

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

    // Initialize test register values
    data[0] = 103;  // Wind speed: 10.3 km/hr
    data[1] = 0;    // Wind direction: 0 degrees

    // Register worker and start on port 502
    MBserver.registerWorker(1, READ_HOLD_REGISTER, &FC03);
    MBserver.start(502, 1, 20000);
}

void loop()
{
    static unsigned long lastMillis = 0;
    if(millis() - lastMillis > 10000)
    {
        lastMillis = millis();
        Serial.printf("free heap: %d\n", ESP.getFreeHeap());
    }
}
