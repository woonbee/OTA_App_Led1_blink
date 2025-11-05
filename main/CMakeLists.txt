idf_component_register(SRCS "OTA_App_Led1_blink.c"
                       INCLUDE_DIRS "."
                       PRIV_REQUIRES esp_https_ota esp_wifi esp_netif nvs_flash driver mbedtls)
