#include <Arduino.h>
#include <WiFi.h>
#include <WiFiUdp.h>
#include <SPIFFS.h>
#include <Preferences.h>
#include <ESPAsyncWebServer.h>
#include <ArduinoJson.h>
#include <ElegantOTA.h>
#include <ESPAsyncE131.h>

// If you already have discovery.{h,cpp} from earlier, keep this include:
#include "discovery.h"

// ---------- PROTOCOL CONSTANTS ----------
//
// We only care about network protocols: ArtNet, E1.31 (sACN), DDP.
// “DMX” in comments elsewhere just means “per-channel 0–255 values”,
// coming from these network packets – no physical DMX output here.
//

// Art-Net
#define ARTNET_SUBNET         0
#define ARTNET_UNIVERSE       41
#define ARTNET_ARTDMX         0x5000
#define ARTNET_ARTPOLL        0x2000
#define ARTNET_PORT           0x1936      // 6454
#define ARTNET_START_ADDRESS  18          // first channel byte in ArtNet packet

// E1.31 (sACN)
#define E131_SUBNET           0
#define E131_PORT             5568
#define E131_START_ADDRESS    126         // first channel byte in E1.31 packet

// DDP
#define DDP_PORT              4048
#define DDP_HEADER_LEN        10          // payload starts at byte 10

// Misc
#define ETHERNET_BUFFER_MAX   640
#define STATUS_LED            2
#define NUM_RELAYS            8

// ---------- WiFi CONFIG (change these) ----------

const char* WIFI_SSID = "xlights";
const char* WIFI_PASS = "christmas2024";

// ---------- CONFIG / STATE STRUCTS ----------

struct RelayConfig {
    uint8_t gpio;   // GPIO number for this relay
};

struct DeviceConfig {
    uint16_t universe;     // informational only (we no longer care about “DMX universe” semantics)
    uint16_t startChan;    // informational only
    RelayConfig relays[NUM_RELAYS];
    char ssid[32];
    char pass[32];
};

DeviceConfig cfg;
Preferences prefs;

// High-level trigger: HIGH = ON, LOW = OFF
bool relayState[NUM_RELAYS] = { false };

// ---------- UDP / PACKET STATE ----------

WiFiUDP aUDP;   // Art-Net
WiFiUDP suUDP;  // E1.31 (sACN)
WiFiUDP ddpUDP; // DDP
uint8_t packetBuffer[ETHERNET_BUFFER_MAX];
// Async E1.31 receiver (E1.31 / sACN)
ESPAsyncE131 e131(10);   // queue depth = 10 packets

// Timeout logic
volatile byte currentcounter = 0;
byte   previouscounter       = 0;
unsigned long currentDelay   = 0;

// ---------- RELAY HELPERS (HIGH-LEVEL TRIGGER) ----------

void setRelay(uint8_t index, bool on) {
    if (index >= NUM_RELAYS) return;
    uint8_t gpio = cfg.relays[index].gpio;
    if (gpio == 0xFF || gpio == 0) return;   // unmapped / disabled

    pinMode(gpio, OUTPUT);
    digitalWrite(gpio, on ? HIGH : LOW);     // HIGH = ON, LOW = OFF
    relayState[index] = on;
}

void setAllRelays(bool on) {
    for (uint8_t i = 0; i < NUM_RELAYS; i++) {
        setRelay(i, on);
    }
}

// ---------- NVS CONFIG (GPIO + WiFi) ----------

void loadCfg() {
    // Defaults
    cfg.universe  = ARTNET_UNIVERSE;
    cfg.startChan = 1;

    // ESP32-safe default pins (adjust if you like)
    uint8_t defPins[NUM_RELAYS] = {26, 25, 27, 14, 33, 32, 13, 12};
    for (int i = 0; i < NUM_RELAYS; i++) {
        cfg.relays[i].gpio = defPins[i];
    }

    strncpy(cfg.ssid, WIFI_SSID, sizeof(cfg.ssid) - 1);
    strncpy(cfg.pass, WIFI_PASS, sizeof(cfg.pass) - 1);

    prefs.begin("cfg", true);

    cfg.universe  = prefs.getUShort("u", cfg.universe);
    cfg.startChan = prefs.getUShort("s", cfg.startChan);

    for (int i = 0; i < NUM_RELAYS; i++) {
        char key[8];
        sprintf(key, "g%u", i);
        uint8_t pin = prefs.getUChar(key, cfg.relays[i].gpio);
        cfg.relays[i].gpio = pin;
    }

    char ssidBuf[32] = {0};
    char passBuf[32] = {0};
    if (prefs.getString("ssid", ssidBuf, sizeof(ssidBuf)) > 0) {
        strncpy(cfg.ssid, ssidBuf, sizeof(cfg.ssid) - 1);
    }
    if (prefs.getString("pass", passBuf, sizeof(passBuf)) > 0) {
        strncpy(cfg.pass, passBuf, sizeof(cfg.pass) - 1);
    }

    prefs.end();
}

void saveCfg() {
    prefs.begin("cfg", false);

    prefs.putUShort("u", cfg.universe);
    prefs.putUShort("s", cfg.startChan);

    for (int i = 0; i < NUM_RELAYS; i++) {
        char key[8];
        sprintf(key, "g%u", i);
        prefs.putUChar(key, cfg.relays[i].gpio);
    }

    prefs.putString("ssid", cfg.ssid);
    prefs.putString("pass", cfg.pass);

    prefs.end();
}

// ---------- WiFi ----------

void wifiConnect() {
    WiFi.mode(WIFI_STA);
    WiFi.begin(cfg.ssid, cfg.pass);

    Serial.printf("Connecting to WiFi SSID '%s'", cfg.ssid);
    uint8_t tries = 0;
    while (WiFi.status() != WL_CONNECTED && tries < 60) {
        delay(250);
        Serial.print(".");
        tries++;
    }
    Serial.println();

    if (WiFi.status() == WL_CONNECTED) {
        Serial.print("WiFi connected, IP: ");
        Serial.println(WiFi.localIP());
        // No WiFi.enableMulticast() on ESP32 core
    } else {
        Serial.println("WiFi connect FAILED, working offline.");
    }
}

// ---------- WEB / API / UI ----------

AsyncWebServer server(80);

void startWeb() {
    // Advanced GUI
    server.on("/", HTTP_GET, [](AsyncWebServerRequest *request) {
        request->send(SPIFFS, "/ui.html", "text/html");
    });

    // Config + relay state for UI
    // Config + relay state for UI
    server.on("/api/config", HTTP_GET, [](AsyncWebServerRequest *request) {
        DynamicJsonDocument doc(1024);

        // Informational only – you don't care about DMX, just network protocols
        doc["protocols"] = "ArtNet / E1.31 / DDP";
        doc["channels"]  = NUM_RELAYS;

        // For your banner
        doc["xlights_discovery"] = true;

        // Kept for backward compatibility with older UIs
        doc["universe"]  = cfg.universe;
        doc["startChan"] = cfg.startChan;

        JsonArray arr = doc.createNestedArray("relays");
        for (uint8_t i = 0; i < NUM_RELAYS; i++) {
            JsonObject o = arr.createNestedObject();
            o["index"] = i;
            o["gpio"]  = cfg.relays[i].gpio;
            o["state"] = relayState[i];
        }

        String out;
        serializeJsonPretty(doc, out);
        request->send(200, "application/json", out);
    });


    // Manual relay control from UI: POST relay=<n>&value=0|1
    server.on("/api/set", HTTP_POST, [](AsyncWebServerRequest *request) {
        if (request->hasParam("relay", true) && request->hasParam("value", true)) {
            int  idx = request->getParam("relay", true)->value().toInt();
            bool val = (request->getParam("value", true)->value() == "1");

            if (idx >= 0 && idx < NUM_RELAYS) {
                setRelay((uint8_t)idx, val);
                request->send(200, "text/plain", "OK");
                return;
            }
        }
        request->send(400, "text/plain", "Bad params");
    });

    // GPIO mapping from UI: POST relay=<n>&gpio=<pin>
    server.on("/api/set_gpio", HTTP_POST, [](AsyncWebServerRequest *request) {
        if (request->hasParam("relay", true) && request->hasParam("gpio", true)) {
            int idx  = request->getParam("relay", true)->value().toInt();
            int gpio = request->getParam("gpio",  true)->value().toInt();

            if (idx < 0 || idx >= NUM_RELAYS) {
                request->send(400, "text/plain", "Invalid relay index");
                return;
            }
            if (gpio < 0 || gpio > 39) {
                request->send(400, "text/plain", "Invalid GPIO");
                return;
            }

            cfg.relays[idx].gpio = (uint8_t)gpio;
            saveCfg();
            setRelay((uint8_t)idx, relayState[idx]);   // apply current state on new pin

            request->send(200, "text/plain", "OK");
            Serial.printf("Relay %d remapped to GPIO %d\n", idx, gpio);
            return;
        }
        request->send(400, "text/plain", "Missing relay/gpio");
    });

    // OTA
    ElegantOTA.begin(&server);

    server.begin();
    Serial.println("HTTP server started");
}

// ---------- ArtNet / E1.31 / DDP HANDLERS ----------

// ArtNet opcode
int artNetOpCode(uint8_t* pbuff) {
    String test = String((char*)pbuff);
    if (test.equals("Art-Net")) {
        if (pbuff[11] >= 14) {
            return pbuff[9] * 256 + pbuff[8];  // lo byte first
        }
    }
    return 0;
}

// ArtNet → relays
void artDMXReceived(uint8_t* pbuff) {
    if ((pbuff[14] & 0xF) == ARTNET_UNIVERSE) {
        if ((pbuff[14] >> 8) == ARTNET_SUBNET) {
            int channelOffset = 0;
            for (int b = 0; b < NUM_RELAYS; b++) {
                uint8_t val = pbuff[ARTNET_START_ADDRESS + channelOffset];
                bool on = (val > 127);
                setRelay(b, on);
                channelOffset++;
            }
        }
    }
}


// DDP → relays
void ddpReceived(uint8_t* buf, int len) {
    if (len <= DDP_HEADER_LEN) return;

    // Very lightweight parse: default DDP header is 10 bytes.
    // Byte 2..5: 32-bit big-endian offset
    // Byte 6..7: 16-bit big-endian data length
    uint32_t offset = ((uint32_t)buf[2] << 24) |
                      ((uint32_t)buf[3] << 16) |
                      ((uint32_t)buf[4] << 8)  |
                      (uint32_t)buf[5];

    uint16_t dataLen = ((uint16_t)buf[6] << 8) | (uint16_t)buf[7];

    // Clamp to actual packet size
    if (DDP_HEADER_LEN + dataLen > len) {
        dataLen = len - DDP_HEADER_LEN;
    }

    // We assume 1 byte per “channel” (no RGB unpacking here).
    // First 8 channels coming from DDP control our 8 relays.
    // offset is the starting channel index in the global stream.
    for (int i = 0; i < NUM_RELAYS; i++) {
        uint32_t chanIndex = offset + i;
        if (chanIndex >= dataLen) break;

        uint8_t v = buf[DDP_HEADER_LEN + chanIndex];
        bool on = (v > 127);
        setRelay((uint8_t)i, on);
    }
}

// Unified handler for all three protocols
void handlePackets() {
    // Timeout logic
    if (currentcounter != previouscounter) {
        currentDelay = millis();
        previouscounter = currentcounter;
    }
    if (millis() - currentDelay > 30000) {
        digitalWrite(STATUS_LED, LOW);   // not receiving
        // Optionally: setAllRelays(false);
    }

    // 1) Art-Net
    int packetSize = aUDP.parsePacket();
    if (packetSize > 0) {
        if (packetSize > ETHERNET_BUFFER_MAX) packetSize = ETHERNET_BUFFER_MAX;
        aUDP.read(packetBuffer, packetSize);

        int opcode = artNetOpCode(packetBuffer);
        if (opcode == ARTNET_ARTDMX) {
            Serial.println("ArtNet Packet Received");
            artDMXReceived(packetBuffer);
            currentcounter++;
            digitalWrite(STATUS_LED, HIGH);
        }
        return;
    }

    // 2) E1.31 (sACN) via ESPAsyncE131
    while (!e131.isEmpty()) {
        e131_packet_t packet;
        e131.pull(&packet);

        // property_values[0] = start code
        // property_values[1..] = DMX channels 1..N
        Serial.println("E131 Packet Received (ESPAsyncE131)");

        for (int i = 0; i < NUM_RELAYS; i++) {
            // cfg.startChan is 1-based DMX start channel
            uint16_t chan = cfg.startChan + i;  // 1..n
            if (chan >= packet.property_value_count) {
                break;
            }

            uint8_t v = packet.property_values[chan];
            bool on = (v > 127);
            setRelay((uint8_t)i, on);
        }

        currentcounter++;
        digitalWrite(STATUS_LED, HIGH);
        // NO return here – we let DDP still be processed in same loop if needed
    }


    // 3) DDP
    packetSize = ddpUDP.parsePacket();
    if (packetSize > 0) {
        if (packetSize > ETHERNET_BUFFER_MAX) packetSize = ETHERNET_BUFFER_MAX;
        ddpUDP.read(packetBuffer, packetSize);

        Serial.println("DDP Packet Received");
        Serial.print("BUFFER" + packetBuffer[packetSize]);
        ddpReceived(packetBuffer, packetSize);
        currentcounter++;
        digitalWrite(STATUS_LED, HIGH);
        return;
    }
}

// ---------- Power On Self Test ----------

void POST() {
    Serial.println("POST: walking the relays");
    for (int i = 0; i < NUM_RELAYS; i++) {
        setRelay(i, true);
        delay(300);
        setRelay(i, false);
    }
    Serial.println("POST Complete");
}

// ---------- SETUP / LOOP ----------

void setup() {
    Serial.begin(115200);
    delay(200);
    Serial.println();
    Serial.println("ESP32 WiFi Relay Controller (ArtNet / E1.31 / DDP)");

    pinMode(STATUS_LED, OUTPUT);
    digitalWrite(STATUS_LED, LOW);

    if (!SPIFFS.begin(true)) {
        Serial.println("SPIFFS mount failed!");
    }

    loadCfg();
    wifiConnect();


    // Start UDP listeners
    aUDP.begin(ARTNET_PORT);
    ddpUDP.begin(DDP_PORT);

    // Start async E1.31 listener (multicast)
    // Uses cfg.universe from main_config.h and joins 1 universe.
    if (e131.begin(E131_MULTICAST, cfg.universe, 1)) {
        Serial.printf("E1.31 listening (multicast), universe %u\n", cfg.universe);
    } else {
        Serial.println("E1.31 init FAILED");
    }

    Serial.printf("Listening for Art-Net on port %u\n", ARTNET_PORT);
    Serial.printf("Listening for DDP on port %u\n", DDP_PORT);


    // Initialize relays to OFF
    setAllRelays(false);

    // Self-test
    POST();

    // Web UI + OTA
    startWeb();

    // xLights & FPP discovery
    startXLightsDiscovery();


    currentDelay = millis();
}

void loop() {
    handlePackets();    // ArtNet / E1.31 / DDP
    handleXLightsDiscovery();  // <--- add this
    ElegantOTA.loop();  // if you kept OTA
    delay(1);

}
