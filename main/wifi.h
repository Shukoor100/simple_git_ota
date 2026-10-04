#pragma once
#include <stdbool.h>
#include "freertos/FreeRTOS.h"

/* Starts Wi-Fi. If no credentials are saved, starts SoftAP provisioning
 * (non-blocking). Requires NVS, esp_netif and the default event loop. */
void wifi_init(void);

/* Blocks until the station has an IP address. Returns true if connected. */
bool wifi_wait_connected(TickType_t timeout);
