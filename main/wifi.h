#pragma once
#include <stdbool.h>
#include "freertos/FreeRTOS.h"

/* Starts Wi-Fi. If no credentials are saved, starts the "MyOTA" access point
 * with a setup page (prov.html) at http://192.168.4.1 (non-blocking).
 * Requires NVS, esp_netif and the default event loop. */
void wifi_init(void);

/* Blocks until the station has an IP address. Returns true if connected. */
bool wifi_wait_connected(TickType_t timeout);
