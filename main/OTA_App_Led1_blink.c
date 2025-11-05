#include "esp_https_ota.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_crt_bundle.h"
#include "freertos/event_groups.h"
#include "driver/gpio.h"
#include "freertos/task.h"
#include "nvs.h"
#include <string.h>

#define WIFI_SSID "SK_WiFiGIGA3B22_2.4G"
#define WIFI_PASS "AMT0A@9063"
#define LED_PIN 5   // D5 (LED1)

static const char *TAG = "LED1_OTA";
static EventGroupHandle_t wifi_event_group;
#define WIFI_CONNECTED_BIT BIT0

#define DEFAULT_OTA_URL "https://github.com/woonbee/OTA_App_Led1_blink/releases/download/v1.1/OTA_App_Led1_blink.bin"

/* ===== NVS 저장/로드 ===== */
void save_ota_url(const char *url)
{
    nvs_handle_t nvs;
    if (nvs_open("storage", NVS_READWRITE, &nvs) == ESP_OK) {
        nvs_set_str(nvs, "ota_url", url);
        nvs_commit(nvs);
        nvs_close(nvs);
        ESP_LOGI(TAG, "Saved OTA URL: %s", url);
    }
}

bool load_ota_url(char *url_buf, size_t buf_len)
{
    nvs_handle_t nvs;
    if (nvs_open("storage", NVS_READONLY, &nvs) != ESP_OK) return false;
    size_t required_len = buf_len;
    esp_err_t err = nvs_get_str(nvs, "ota_url", url_buf, &required_len);
    nvs_close(nvs);
    return (err == ESP_OK);
}

/* ===== Wi-Fi 이벤트 ===== */
static void wifi_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) esp_wifi_connect();
    else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        esp_wifi_connect();
        ESP_LOGW(TAG, "WiFi disconnected. Reconnecting...");
    }
    else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP)
        xEventGroupSetBits(wifi_event_group, WIFI_CONNECTED_BIT);
}

/* ===== Wi-Fi 초기화 ===== */
void wifi_init(void)
{
    wifi_event_group = xEventGroupCreate();
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
                                                        ESP_EVENT_ANY_ID,
                                                        &wifi_event_handler,
                                                        NULL,
                                                        NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT,
                                                        IP_EVENT_STA_GOT_IP,
                                                        &wifi_event_handler,
                                                        NULL,
                                                        NULL));

    wifi_config_t wifi_config = {0};
    strcpy((char *)wifi_config.sta.ssid, WIFI_SSID);
    strcpy((char *)wifi_config.sta.password, WIFI_PASS);
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "Connecting WiFi...");
    xEventGroupWaitBits(wifi_event_group, WIFI_CONNECTED_BIT, pdFALSE, pdTRUE, portMAX_DELAY);
}

/* ===== LED Task (항상 깜빡임) ===== */
void blink_led_task(void *pvParameters)
{
    gpio_reset_pin(LED_PIN);
    gpio_set_direction(LED_PIN, GPIO_MODE_OUTPUT);

    while (1) {
        gpio_set_level(LED_PIN, 1);
        vTaskDelay(pdMS_TO_TICKS(500));
        gpio_set_level(LED_PIN, 0);
        vTaskDelay(pdMS_TO_TICKS(500));
    }
}

/* ===== OTA Task (주기적으로 확인) ===== */
void ota_task(void *pvParameters)
{
    while (1) {
        char ota_url[256];
        if (!load_ota_url(ota_url, sizeof(ota_url))) {
            ESP_LOGW(TAG, "No OTA URL in NVS. Using default.");
            strcpy(ota_url, DEFAULT_OTA_URL);
            save_ota_url(ota_url);
        }

        ESP_LOGI(TAG, "Starting OTA from: %s", ota_url);

        esp_http_client_config_t http_cfg = {
            .url = ota_url,
            .cert_pem = NULL,
            .crt_bundle_attach = esp_crt_bundle_attach,
            .skip_cert_common_name_check = true,
            .timeout_ms = 60000,
            .buffer_size = 2048,
            .buffer_size_tx = 2048,
        };

        esp_https_ota_config_t ota_cfg = {
            .http_config = &http_cfg,
        };

        esp_err_t ret = esp_https_ota(&ota_cfg);
        if (ret == ESP_OK) {
            ESP_LOGI(TAG, "✅ OTA Success! Rebooting...");
            esp_restart();
        } else {
            ESP_LOGW(TAG, "No update or failed: %s", esp_err_to_name(ret));
        }

        // 60초마다 OTA 재시도
        vTaskDelay(pdMS_TO_TICKS(60000));
    }
}

/* ===== 메인 함수 ===== */
void app_main(void)
{
    ESP_ERROR_CHECK(nvs_flash_init());
    wifi_init();

    ESP_LOGI(TAG, "==========================================");
    ESP_LOGI(TAG, "Start LED1 blinking FW by OTA updating...");
    ESP_LOGI(TAG, "==========================================");

    xTaskCreate(blink_led_task, "blink_led_task", 2048, NULL, 5, NULL);
    xTaskCreate(ota_task, "ota_task", 8192, NULL, 4, NULL);
}
