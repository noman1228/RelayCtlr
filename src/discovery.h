
#pragma once

// Start UDP listener(s) for discovery (xLights / FPP style)
void startXLightsDiscovery();

// Poll / process incoming discovery packets (call from loop())
void handleXLightsDiscovery();