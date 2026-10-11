#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "driver/gpio.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "nvs_flash.h"

#include "ota.h"
#include "wifi.h"

#define BUILD_IN_LED    2

static const char *TAG = "app";

void app_main(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    ota_init();
    wifi_init();
    ota_start_task();   /* keep these three calls in EVERY build you ever ship */

    /* ===== your application code goes here ===== */
    const gpio_num_t led = (gpio_num_t)BUILD_IN_LED;
    gpio_reset_pin(led);
    gpio_set_direction(led, GPIO_MODE_OUTPUT);

    int level = 0;
    int ticks = 0;
    for (;;) {
        level = !level;
        gpio_set_level(led, level);

        if (++ticks % 20 == 0) {   /* every 10 s at 500 ms per step */
            ESP_LOGI(TAG, "alive, firmware v%lu", (unsigned long)ota_running_version());
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
