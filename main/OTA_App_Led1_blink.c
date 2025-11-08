#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_crt_bundle.h"
#include "freertos/event_groups.h"
#include "driver/gpio.h"
#include "freertos/task.h"
#include "esp_ota_ops.h"
#include "nvs.h"
#include <string.h>

#define WIFI_SSID "SK_WiFiGIGA3B22_2.4G"
#define WIFI_PASS "AMT0A@9063"
#define LED_PIN   5   // LED1 핀 번호

static const char *TAG = "LED1_FW";
static EventGroupHandle_t wifi_event_group;
#define WIFI_CONNECTED_BIT BIT0

#define DEFAULT_OTA_URL "https://woonbee.github.io/OTA_App_Led1_blink/OTA_App_Led1_blink.bin"

/* ===========================================================
 * NVS: OTA URL 저장/로드  (Factory와 공유)
 * =========================================================== */
static void save_ota_url(const char *url)
{
    nvs_handle_t nvs;
    if (nvs_open("storage", NVS_READWRITE, &nvs) == ESP_OK) {
        nvs_set_str(nvs, "ota_url", url);
        nvs_commit(nvs);
        nvs_close(nvs);
        ESP_LOGI(TAG, "Saved OTA URL: %s", url);
    } else {
        ESP_LOGE(TAG, "Failed to open NVS(storage) for ota_url");
    }
}

static bool load_ota_url(char *url_buf, size_t buf_len)
{
    nvs_handle_t nvs;
    if (nvs_open("storage", NVS_READONLY, &nvs) != ESP_OK) {
        return false;
    }
    size_t required_len = buf_len;
    esp_err_t err = nvs_get_str(nvs, "ota_url", url_buf, &required_len);
    nvs_close(nvs);
    return (err == ESP_OK);
}

/* ===========================================================
 * A: 다음 부팅 순서를 Factory FW로 설정
 *    - otadata 파티션에 저장됨 (부트로더 관리 영역)
 * =========================================================== */
static void set_next_boot_to_factory(void)
{
    const esp_partition_t *factory =
        esp_partition_find_first(ESP_PARTITION_TYPE_APP,
                                 ESP_PARTITION_SUBTYPE_APP_FACTORY,
                                 NULL);
    if (!factory) {
        ESP_LOGE(TAG, "Factory partition not found");
        return;
    }

    esp_err_t err = esp_ota_set_boot_partition(factory);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Next boot is set to Factory FW (%s)", factory->label);
    } else {
        ESP_LOGE(TAG, "Failed to set next boot to Factory: %s", esp_err_to_name(err));
    }
}

/* ===========================================================
 * B: 현재 실행 중인 OTA 파티션을 NVS에 저장
 *    - "boot_info" / "last_boot" 키에 기록 (Factory 참고용)
 * =========================================================== */
static void save_last_boot_partition(void)
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    if (!running) {
        ESP_LOGE(TAG, "Failed to get running partition");
        return;
    }

    // OTA PENDING 상태였다면, 여기서 정상 부팅 완료 처리
    esp_err_t err = esp_ota_mark_app_valid_cancel_rollback();
    if (err != ESP_OK && err != ESP_ERR_OTA_BASE) {
        // ESP_ERR_OTA_BASE 계열은 "pending 상태가 아니었다" 정도라 치명적 아님
        ESP_LOGW(TAG, "mark_app_valid_cancel_rollback: %s", esp_err_to_name(err));
    }

    nvs_handle_t nvs;
    err = nvs_open("boot_info", NVS_READWRITE, &nvs);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open NVS(boot_info): %s", esp_err_to_name(err));
        return;
    }

    err = nvs_set_str(nvs, "last_boot", running->label);
    if (err == ESP_OK) {
        nvs_commit(nvs);
        ESP_LOGI(TAG, "Saved last_boot partition: %s", running->label);
    } else {
        ESP_LOGE(TAG, "Failed to set last_boot: %s", esp_err_to_name(err));
    }
    nvs_close(nvs);
}

/* ===========================================================
 * Wi-Fi 이벤트 / 초기화 (기존 코드 유지)
 * =========================================================== */
static void wifi_event_handler(void *arg,
                               esp_event_base_t event_base,
                               int32_t event_id,
                               void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT &&
               event_id == WIFI_EVENT_STA_DISCONNECTED) {
        ESP_LOGW(TAG, "WiFi disconnected. Reconnecting...");
        esp_wifi_connect();
    } else if (event_base == IP_EVENT &&
               event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&event->ip_info.ip));
        xEventGroupSetBits(wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

static void wifi_init(void)
{
    wifi_event_group = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID,
        &wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP,
        &wifi_event_handler, NULL, NULL));

    wifi_config_t wifi_config = {0};
    strcpy((char *)wifi_config.sta.ssid, WIFI_SSID);
    strcpy((char *)wifi_config.sta.password, WIFI_PASS);

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "Connecting WiFi...");
    xEventGroupWaitBits(wifi_event_group,
                        WIFI_CONNECTED_BIT,
                        pdFALSE, pdTRUE,
                        portMAX_DELAY);
}

/* ===========================================================
 * LED Task (기존 코드 유지)
 * =========================================================== */
static void blink_led_task(void *pvParameters)
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

/* ===========================================================
 * OTA URL 체크 Task (기존 코드 유지)
 * =========================================================== */
static void check_url_task(void *pvParameters)
{
    char saved_url[256] = {0};
    bool has_saved = load_ota_url(saved_url, sizeof(saved_url));

    if (!has_saved) {
        ESP_LOGW(TAG, "No OTA URL in NVS. Saving default URL...");
        save_ota_url(DEFAULT_OTA_URL);
    } else if (strcmp(saved_url, DEFAULT_OTA_URL) != 0) {
        ESP_LOGI(TAG, "OTA URL changed. Updating NVS...");
        save_ota_url(DEFAULT_OTA_URL);
    } else {
        ESP_LOGI(TAG, "OTA URL is up-to-date.");
    }

    vTaskDelete(NULL);
}

/* ===========================================================
 * app_main
 * =========================================================== */
void app_main(void)
{
    ESP_ERROR_CHECK(nvs_flash_init());

    ESP_LOGI(TAG, "==========================================");
    ESP_LOGI(TAG, " Start LED1 Blink Firmware (OTA APP)");
    ESP_LOGI(TAG, "==========================================");

    /* 🔴 A: 항상 다음 부팅은 Factory로 (부트로더용 OTA데이터에 기록) */
    set_next_boot_to_factory();

    /* 🔵 B: 현재 부팅한 OTA 파티션을 NVS(boot_info)에 기록 */
    save_last_boot_partition();

    /* 이후부터는 기존 기능 그대로 */
    wifi_init();

    xTaskCreate(blink_led_task,  "blink_led_task",  2048, NULL, 5, NULL);
    xTaskCreate(check_url_task,  "check_url_task",  4096, NULL, 4, NULL);
}
