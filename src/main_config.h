#pragma once
#include <stdint.h>

struct RelayConfig {
    uint8_t gpio;
};

struct DeviceConfig {
    uint16_t universe;
    uint16_t startChan;
    RelayConfig relays[8];

    // WiFi credentials used in main.cpp
    char ssid[32];
    char pass[32];
};

extern DeviceConfig cfg;
