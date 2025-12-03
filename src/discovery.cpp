#include <WiFi.h>
#include <WiFiUdp.h>
#include <ArduinoJson.h>

#include "main_config.h"   // for DeviceConfig cfg
#include "discovery.h"

// Global config object from main_config.h
extern DeviceConfig cfg;

// UDP socket for discovery
static WiFiUDP g_discUdp;

// FPP / xLights discovery port
// (FPPDiscovery subscribes to broadcast + multicast on 32320)
static const uint16_t FPP_DISCOVERY_PORT = 32320;

// Small buffer for incoming packets
static uint8_t g_discBuf[512];

// Helper: build discovery JSON and send back to the requester
static void sendDiscoveryReply(const IPAddress &remoteIP, uint16_t remotePort)
{
    DynamicJsonDocument doc(512);

    // xLights expects EXACTLY this:
    doc["type"]     = "ESPixelStick";
    doc["vendor"]   = "ESPixelStick";
    doc["model"]    = "ESPixelStick-4.x";
    doc["variant"]  = "ESP32";
    doc["version"]  = "1.0.0";

    const char *host = WiFi.getHostname() ? WiFi.getHostname() : "esp32-relay";
    doc["name"]     = host;
    doc["hostname"] = host;
    doc["addr"]     = WiFi.localIP().toString();

    // Supported protocols
    JsonObject proto = doc.createNestedObject("protocols");
    proto["e131"]   = true;
    proto["artnet"] = true;
    proto["ddp"]    = true;

    // Outputs array (MUST EXIST)
    JsonArray outputs = doc.createNestedArray("outputs");
    JsonObject out    = outputs.createNestedObject();
    out["type"]       = "DDP";         // or "e1.31" but DDP matches your setup
    out["channel_start"] = cfg.startChan;
    out["channel_count"] = NUM_RELAYS;
    out["universe"]       = cfg.universe;
    out["universe_count"] = 1;

    String outStr;
    serializeJson(doc, outStr);

    Serial.printf("[DISCOVERY] Reply -> %s:%u : %s\n",
                  remoteIP.toString().c_str(), remotePort, outStr.c_str());

    g_discUdp.beginPacket(remoteIP, remotePort);
    g_discUdp.write((const uint8_t *)outStr.c_str(), outStr.length());
    g_discUdp.endPacket();
}
void startXLightsDiscovery()
{
    // Listen on FPP/xLights discovery port 32320 on all interfaces
    if (!g_discUdp.begin(FPP_DISCOVERY_PORT)) {
        Serial.println("[DISCOVERY] FAILED to open UDP port 32320");
        return;
    }

    Serial.print("[DISCOVERY] Listening for discovery on UDP port ");
    Serial.println(FPP_DISCOVERY_PORT);

    // We are NOT using WiFi.enableMulticast() – ESP32 WiFiClass does
    // not have that method. If you later need multicast membership
    // for 239.70.80.80:32320, we’ll switch this to AsyncUDP.
}

// Call this from loop()
void handleXLightsDiscovery()
{
    int packetSize = g_discUdp.parsePacket();
    if (!packetSize)
        return;

    if (packetSize > (int)sizeof(g_discBuf))
        packetSize = sizeof(g_discBuf);

    int len = g_discUdp.read(g_discBuf, packetSize);
    if (len <= 0)
        return;

    IPAddress remoteIP   = g_discUdp.remoteIP();
    uint16_t remotePort  = g_discUdp.remotePort();

    Serial.print("[DISCOVERY] Packet from ");
    Serial.print(remoteIP);
    Serial.print(":");
    Serial.print(remotePort);
    Serial.print(" len=");
    Serial.println(len);

    // For now we don't try to parse the query; we just assume any packet
    // on 32320 is a discovery request and answer with our JSON.
    sendDiscoveryReply(remoteIP, remotePort);
}
