#pragma once
#include <stdint.h>

constexpr uint8_t NUM_RELAYS = 16;

struct RelayConfig {
    uint8_t gpio;
};

struct DeviceConfig {
    uint16_t universe;
    uint16_t startChan;
    RelayConfig relays[NUM_RELAYS];

    // WiFi credentials used in main.cpp
    char ssid[32];
    char pass[32];
};

extern DeviceConfig cfg;
