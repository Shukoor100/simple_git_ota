idf_component_register(
    SRCS "main.c" "wifi.c" "ota.c"
    INCLUDE_DIRS "."
    PRIV_REQUIRES esp_wifi esp_event esp_netif nvs_flash esp_http_server
                  esp_http_client esp_https_ota esp-tls app_update driver
    EMBED_TXTFILES "prov.html"
)
