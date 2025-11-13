#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "esp_log.h"

static const char* TAG = "app_main_example";

void app_main(void)
{
    esp_log_level_set("*", ESP_LOG_INFO);
    ESP_LOGI(TAG, "Hello — building with local Arduino component (if override works).");
    for(;;) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
