#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"

#include "driver/gpio.h"
#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"

#include "wifi.h"

#define PORTAL_SSID      "MyOTA"
#define PORTAL_PASSWORD  "12345678"
#define MAX_CONNECT_TRIES 5   /* failed attempts before reopening the portal */

static const char *TAG = "wifi";
#define CONNECTED_BIT BIT0
static EventGroupHandle_t s_events;

static httpd_handle_t s_server;
static bool s_portal_active;
static bool s_have_creds;
static bool s_ever_connected;
static int  s_retries;

extern const char prov_html_start[] asm("_binary_prov_html_start");

static void start_portal(void);

/* ---------- HTTP portal ---------- */

static esp_err_t index_get_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, prov_html_start, HTTPD_RESP_USE_STRLEN);
}

/* Anything else (captive-portal probes, favicon...) goes back to the form. */
static esp_err_t not_found_handler(httpd_req_t *req, httpd_err_code_t err)
{
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "http://192.168.4.1/");
    return httpd_resp_send(req, NULL, 0);
}

/* Decodes the value of `key` from an application/x-www-form-urlencoded body. */
static bool form_get(const char *body, const char *key, char *out, size_t out_len)
{
    size_t klen = strlen(key);
    const char *p = body;
    while (p && *p) {
        if (strncmp(p, key, klen) == 0 && p[klen] == '=') {
            p += klen + 1;
            size_t n = 0;
            while (*p && *p != '&') {
                char c = *p;
                if (c == '+') {
                    c = ' ';
                } else if (c == '%' && p[1] && p[2]) {
                    char hex[3] = { p[1], p[2], 0 };
                    c = (char)strtol(hex, NULL, 16);
                    p += 2;
                }
                if (n + 1 >= out_len) {
                    return false;
                }
                out[n++] = c;
                p++;
            }
            out[n] = 0;
            return true;
        }
        p = strchr(p, '&');
        if (p) {
            p++;
        }
    }
    return false;
}

static void portal_stop_task(void *arg)
{
    vTaskDelay(pdMS_TO_TICKS(3000));  /* let the browser finish first */
    if (s_server) {
        httpd_stop(s_server);
        s_server = NULL;
    }
    s_portal_active = false;
    esp_wifi_set_mode(WIFI_MODE_STA);
    ESP_LOGI(TAG, "Provisioning portal closed");
    vTaskDelete(NULL);
}

static esp_err_t save_post_handler(httpd_req_t *req)
{
    char body[256];
    if (req->content_len == 0 || req->content_len >= sizeof(body)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Request too large");
        return ESP_FAIL;
    }
    int got = 0;
    while (got < (int)req->content_len) {
        int r = httpd_req_recv(req, body + got, req->content_len - got);
        if (r <= 0) {
            httpd_resp_send_err(req, HTTPD_408_REQ_TIMEOUT, "Receive failed");
            return ESP_FAIL;
        }
        got += r;
    }
    body[got] = 0;

    wifi_config_t cfg = { 0 };
    char ssid[sizeof(cfg.sta.ssid) + 1] = { 0 };
    char pass[sizeof(cfg.sta.password) + 1] = { 0 };
    if (!form_get(body, "ssid", ssid, sizeof(ssid)) || ssid[0] == 0) {
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_send(req, "Please enter the network name.", HTTPD_RESP_USE_STRLEN);
    }
    /* A missing/empty password means an open network; an over-long one is rejected. */
    if (strstr(body, "password=") != NULL && !form_get(body, "password", pass, sizeof(pass))) {
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_send(req, "Password is too long (max 63 characters).", HTTPD_RESP_USE_STRLEN);
    }

    strlcpy((char *)cfg.sta.ssid, ssid, sizeof(cfg.sta.ssid));
    strlcpy((char *)cfg.sta.password, pass, sizeof(cfg.sta.password));
    cfg.sta.pmf_cfg.capable = true;

    ESP_LOGI(TAG, "Saving Wi-Fi credentials for SSID '%s'", ssid);
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &cfg));  /* persisted to flash */
    s_have_creds = true;
    s_retries = 0;
    esp_wifi_disconnect();
    esp_wifi_connect();

    return httpd_resp_send(req,
        "Saved. The device is connecting to your Wi-Fi and the MyOTA network will turn off. "
        "If MyOTA reappears, the name or password was wrong - try again.",
        HTTPD_RESP_USE_STRLEN);
}

static void start_portal(void)
{
    if (s_portal_active) {
        return;
    }

    wifi_config_t ap = { 0 };
    strlcpy((char *)ap.ap.ssid, PORTAL_SSID, sizeof(ap.ap.ssid));
    strlcpy((char *)ap.ap.password, PORTAL_PASSWORD, sizeof(ap.ap.password));
    ap.ap.ssid_len = strlen(PORTAL_SSID);
    ap.ap.channel = 1;
    ap.ap.max_connection = 4;
    ap.ap.authmode = WIFI_AUTH_WPA2_PSK;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap));
    ESP_ERROR_CHECK(esp_wifi_start());

    httpd_config_t hc = HTTPD_DEFAULT_CONFIG();
    hc.max_uri_handlers = 4;
    hc.lru_purge_enable = true;
    ESP_ERROR_CHECK(httpd_start(&s_server, &hc));

    const httpd_uri_t index_uri = { .uri = "/", .method = HTTP_GET, .handler = index_get_handler };
    const httpd_uri_t save_uri  = { .uri = "/save", .method = HTTP_POST, .handler = save_post_handler };
    httpd_register_uri_handler(s_server, &index_uri);
    httpd_register_uri_handler(s_server, &save_uri);
    httpd_register_err_handler(s_server, HTTPD_404_NOT_FOUND, not_found_handler);

    s_portal_active = true;
    ESP_LOGI(TAG, "Setup portal: join Wi-Fi '%s' (password '%s'), then open http://192.168.4.1",
             PORTAL_SSID, PORTAL_PASSWORD);
}

/* ---------- Wi-Fi events ---------- */

static void event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT) {
        if (id == WIFI_EVENT_STA_START) {
            if (s_have_creds) {
                esp_wifi_connect();
            }
        } else if (id == WIFI_EVENT_STA_DISCONNECTED) {
            xEventGroupClearBits(s_events, CONNECTED_BIT);
            if (!s_have_creds) {
                return;
            }
            s_retries++;
            if (!s_ever_connected && s_retries >= MAX_CONNECT_TRIES) {
                ESP_LOGW(TAG, "Could not connect to saved Wi-Fi, reopening setup portal");
                s_have_creds = false;
                esp_wifi_restore();  /* forget the bad credentials */
                start_portal();
                return;
            }
            ESP_LOGW(TAG, "Disconnected, retrying");
            esp_wifi_connect();
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *ev = (ip_event_got_ip_t *)data;
        ESP_LOGI(TAG, "Got IP " IPSTR, IP2STR(&ev->ip_info.ip));
        s_ever_connected = true;
        s_retries = 0;
        xEventGroupSetBits(s_events, CONNECTED_BIT);
        if (s_portal_active) {
            xTaskCreate(portal_stop_task, "portal_stop", 3072, NULL, 5, NULL);
        }
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

    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &event_handler, NULL));

    esp_netif_create_default_wifi_sta();
    esp_netif_create_default_wifi_ap();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    if (reset_button_held()) {
        ESP_LOGW(TAG, "Reset button held: clearing saved Wi-Fi credentials");
        esp_wifi_restore();
    }

    wifi_config_t saved = { 0 };
    esp_wifi_get_config(WIFI_IF_STA, &saved);
    s_have_creds = saved.sta.ssid[0] != 0;

    if (s_have_creds) {
        ESP_LOGI(TAG, "Using saved Wi-Fi '%s'", (const char *)saved.sta.ssid);
        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
        ESP_ERROR_CHECK(esp_wifi_start());
    } else {
        start_portal();
    }
}

bool wifi_wait_connected(TickType_t timeout)
{
    EventBits_t bits = xEventGroupWaitBits(s_events, CONNECTED_BIT, pdFALSE, pdTRUE, timeout);
    return (bits & CONNECTED_BIT) != 0;
}
