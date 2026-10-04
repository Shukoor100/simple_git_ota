#pragma once
#include <stdint.h>

/* Call early in app_main (after NVS init). Reads the running version and
 * does rollback bookkeeping. */
void ota_init(void);

/* Starts the background task that polls version.txt and updates. */
void ota_start_task(void);

uint32_t ota_running_version(void);
