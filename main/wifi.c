#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"

#include "driver/gpio.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "wifi_provisioning/manager.h"
#include "wifi_provisioning/scheme_softap.h"

#include "wifi.h"

static const char *TAG = "wifi";
#define CONNECTED_BIT BIT0
static EventGroupHandle_t s_events;

static void event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_PROV_EVENT) {
        switch (id) {
        case WIFI_PROV_START:
            ESP_LOGI(TAG, "Provisioning started");
            break;
        case WIFI_PROV_CRED_RECV: {
            wifi_sta_config_t *cfg = (wifi_sta_config_t *)data;
            ESP_LOGI(TAG, "Received credentials for SSID '%s'", (const char *)cfg->ssid);
            break;
        }
        case WIFI_PROV_CRED_FAIL: {
            wifi_prov_sta_fail_reason_t *r = (wifi_prov_sta_fail_reason_t *)data;
            ESP_LOGE(TAG, "Provisioning failed: %s",
                     (*r == WIFI_PROV_STA_AUTH_ERROR) ? "wrong password" : "AP not found");
            wifi_prov_mgr_reset_sm_state_on_failure();
            break;
        }
        case WIFI_PROV_CRED_SUCCESS:
            ESP_LOGI(TAG, "Provisioning successful");
            break;
        case WIFI_PROV_END:
            wifi_prov_mgr_deinit();
            break;
        default:
            break;
        }
    } else if (base == WIFI_EVENT) {
        if (id == WIFI_EVENT_STA_START) {
            esp_wifi_connect();
        } else if (id == WIFI_EVENT_STA_DISCONNECTED) {
            xEventGroupClearBits(s_events, CONNECTED_BIT);
            ESP_LOGW(TAG, "Disconnected, retrying");
            esp_wifi_connect();
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *ev = (ip_event_got_ip_t *)data;
        ESP_LOGI(TAG, "Got IP " IPSTR, IP2STR(&ev->ip_info.ip));
        xEventGroupSetBits(s_events, CONNECTED_BIT);
    }
}

/* True if the reset button is held low for 3 seconds right now. */
static bool reset_button_held(void)
{
    const gpio_num_t pin = (gpio_num_t)CONFIG_PROV_RESET_GPIO;
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << pin,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    gpio_config(&io);
    for (int i = 0; i < 30; i++) {
        if (gpio_get_level(pin)) {
            return false;
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    return true;
}

void wifi_init(void)
{
    s_events = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_PROV_EVENT, ESP_EVENT_ANY_ID, &event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &event_handler, NULL));

    esp_netif_create_default_wifi_sta();
    esp_netif_create_default_wifi_ap();  /* needed by SoftAP provisioning */

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    if (reset_button_held()) {
        ESP_LOGW(TAG, "Reset button held: clearing saved Wi-Fi credentials");
        esp_wifi_restore();
    }

    wifi_prov_mgr_config_t prov_cfg = {
        .scheme = wifi_prov_scheme_softap,
        .scheme_event_handler = WIFI_PROV_EVENT_HANDLER_NONE,
    };
    ESP_ERROR_CHECK(wifi_prov_mgr_init(prov_cfg));

    bool provisioned = false;
    ESP_ERROR_CHECK(wifi_prov_mgr_is_provisioned(&provisioned));

    if (!provisioned) {
        uint8_t mac[6];
        esp_wifi_get_mac(WIFI_IF_STA, mac);
        char service_name[16];
        snprintf(service_name, sizeof(service_name), "PROV_%02X%02X%02X", mac[3], mac[4], mac[5]);

        ESP_LOGI(TAG, "No Wi-Fi saved. Join AP '%s' with the ESP SoftAP Provisioning app, PIN '%s'",
                 service_name, CONFIG_PROV_POP);
        ESP_ERROR_CHECK(wifi_prov_mgr_start_provisioning(WIFI_PROV_SECURITY_1, CONFIG_PROV_POP,
                                                         service_name, NULL));
    } else {
        ESP_LOGI(TAG, "Using saved Wi-Fi credentials");
        wifi_prov_mgr_deinit();
        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
        ESP_ERROR_CHECK(esp_wifi_start());
    }
}

bool wifi_wait_connected(TickType_t timeout)
{
    EventBits_t bits = xEventGroupWaitBits(s_events, CONNECTED_BIT, pdFALSE, pdTRUE, timeout);
    return (bits & CONNECTED_BIT) != 0;
}
