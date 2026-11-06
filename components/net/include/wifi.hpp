// Hub and dot 2026
// Imad Jared Cabrera Trejo
// Melissa Moreno Franca

#ifndef WIFI_HPP
#define WIFI_HPP

#include <stddef.h>

// Fills buf with the JSON served at GET /. It runs in the HTTP server task, so it must be thread safe
using JsonProvider = void (*)(char *buf, size_t len);

// Starts WiFi as a station with the settings from `idf.py menuconfig` -> "hub-dot WiFi".
// It does not block: connection, retries and the connectivity test run in the background.
// With an empty SSID it only prints the MAC and returns.
void wifi_start(const char *hostname, JsonProvider provider);

// True if the settings have an SSID, so wifi_start() brings the network up
bool wifi_configured();

// Waits until the board has an IP. Returns false if the time runs out or WiFi is disabled
bool wifi_wait_connected(int timeout_ms);

#endif
