#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_https_ota.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_random.h"
#include "esp_system.h"
#include "nvs.h"

#include "ota.h"
#include "wifi.h"

static const char *TAG = "ota";

#define NVS_NS         "ota"
#define KEY_EXPECT     "expect"   /* version we just installed and are waiting to confirm */
#define KEY_BAD        "bad"      /* highest version that failed and was rolled back */

static uint32_t s_running_ver;
static uint32_t s_bad_ver;
static bool     s_pending_verify;

static bool parse_version(const char *s, uint32_t *out)
{
    char *end = NULL;
    unsigned long v = strtoul(s, &end, 10);
    if (end == s) {
        return false;
    }
    *out = (uint32_t)v;
    return true;
}

static void nvs_erase_expect(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_erase_key(h, KEY_EXPECT);
        nvs_commit(h);
        nvs_close(h);
    }
}

static void nvs_set_expect(uint32_t ver)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u32(h, KEY_EXPECT, ver);
        nvs_commit(h);
        nvs_close(h);
    }
}

void ota_init(void)
{
    const esp_partition_t *running = esp_ota_get_running_partition();

    esp_app_desc_t desc = {0};
    if (esp_ota_get_partition_description(running, &desc) == ESP_OK) {
        if (!parse_version(desc.version, &s_running_ver)) {
            s_running_ver = 0;
        }
    }

    esp_ota_img_states_t state;
    if (esp_ota_get_state_partition(running, &state) == ESP_OK &&
        state == ESP_OTA_IMG_PENDING_VERIFY) {
        s_pending_verify = true;
    }

    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        uint32_t expect = 0;
        if (nvs_get_u32(h, KEY_EXPECT, &expect) == ESP_OK && s_running_ver < expect) {
            /* We installed `expect` but are running something older: it was rolled back. */
            ESP_LOGW(TAG, "v%lu failed and was rolled back; will not retry it", (unsigned long)expect);
            nvs_set_u32(h, KEY_BAD, expect);
            nvs_erase_key(h, KEY_EXPECT);
            nvs_commit(h);
        }
        nvs_get_u32(h, KEY_BAD, &s_bad_ver);
        nvs_close(h);
    }

    ESP_LOGI(TAG, "Running firmware v%lu (%s), pending verify: %s, blocked version: %lu",
             (unsigned long)s_running_ver, desc.version,
             s_pending_verify ? "yes" : "no", (unsigned long)s_bad_ver);
}

uint32_t ota_running_version(void)
{
    return s_running_ver;
}

/* The new image is only confirmed after it has booted, joined Wi-Fi and
 * successfully reached the update server over HTTPS. If it never gets there
 * (crash, reboot, bad Wi-Fi code), the bootloader rolls back to the old image. */
static void confirm_running_image(void)
{
    if (!s_pending_verify) {
        return;
    }
    if (esp_ota_mark_app_valid_cancel_rollback() == ESP_OK) {
        s_pending_verify = false;
        nvs_erase_expect();
        ESP_LOGI(TAG, "New firmware confirmed");
    }
}

static esp_err_t fetch_remote_version(uint32_t *out)
{
    char url[256];
    snprintf(url, sizeof(url), "%s/version.txt?t=%08lx", CONFIG_OTA_BASE_URL, (unsigned long)esp_random());

    esp_http_client_config_t cfg = {
        .url = url,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 10000,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        return ESP_FAIL;
    }
    esp_http_client_set_header(client, "Cache-Control", "no-cache");

    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "version request failed: %s", esp_err_to_name(err));
        esp_http_client_cleanup(client);
        return err;
    }

    esp_http_client_fetch_headers(client);
    int status = esp_http_client_get_status_code(client);
    if (status != 200) {
        ESP_LOGW(TAG, "version.txt returned HTTP %d", status);
        err = ESP_FAIL;
    } else {
        char buf[16] = {0};
        int n = esp_http_client_read(client, buf, sizeof(buf) - 1);
        if (n <= 0 || !parse_version(buf, out)) {
            ESP_LOGW(TAG, "could not parse version.txt");
            err = ESP_FAIL;
        }
    }

    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    return err;
}

static esp_err_t download_and_install(uint32_t new_ver)
{
    char url[256];
    snprintf(url, sizeof(url), "%s/firmware.bin", CONFIG_OTA_BASE_URL);
    ESP_LOGI(TAG, "Updating to v%lu from %s", (unsigned long)new_ver, url);

    esp_http_client_config_t http_cfg = {
        .url = url,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 15000,
        .keep_alive_enable = true,
    };
    esp_https_ota_config_t ota_cfg = {
        .http_config = &http_cfg,
    };

    esp_err_t err = esp_https_ota(&ota_cfg);
    if (err == ESP_OK) {
        nvs_set_expect(new_ver);
        ESP_LOGI(TAG, "Update written, rebooting");
        vTaskDelay(pdMS_TO_TICKS(1000));
        esp_restart();
    }
    ESP_LOGE(TAG, "Update failed: %s", esp_err_to_name(err));
    return err;
}

static void ota_task(void *arg)
{
    wifi_wait_connected(portMAX_DELAY);
    vTaskDelay(pdMS_TO_TICKS(5000));  /* let the network settle */

    for (;;) {
        uint32_t remote = 0;
        bool ok = (fetch_remote_version(&remote) == ESP_OK);

        if (ok) {
            ESP_LOGI(TAG, "running v%lu, server has v%lu",
                     (unsigned long)s_running_ver, (unsigned long)remote);
            confirm_running_image();
            if (remote > s_running_ver && remote > s_bad_ver) {
                download_and_install(remote);  /* reboots on success */
            }
        }

        uint32_t wait_s = CONFIG_OTA_CHECK_INTERVAL_MIN * 60;
        if (!ok && s_pending_verify) {
            wait_s = 60;  /* unconfirmed image: retry quickly */
        }
        wait_s += esp_random() % 30;  /* jitter so a fleet doesn't hit the server in lockstep */
        vTaskDelay(pdMS_TO_TICKS(wait_s * 1000));
    }
}

void ota_start_task(void)
{
    xTaskCreate(ota_task, "ota_task", 8192, NULL, 5, NULL);
}
