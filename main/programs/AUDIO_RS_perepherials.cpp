#include "headers/audio_read_send.h"
#include <esp_timer.h>
#include "driver/i2s_std.h"
#include "headers/a_c_s.h"
#include "headers/ReciverConfig.h"   // <<--- add this (exact filename may differ)

void AUDIO_RS::I2SReadTrampoline(void* pv)
{
    AUDIO_RS* self = static_cast<AUDIO_RS*>(pv);
    if (!self)
    {
        vTaskDelete(nullptr);
        return;
    }
    self ->I2SReaderLoop();
}

void AUDIO_RS::RingWriterFRMI2STrampoline(void* pv)
{
    AUDIO_RS* self = static_cast<AUDIO_RS*>(pv);
    if (!self)
    {
        vTaskDelete(nullptr);
        return;
    }
    self->RingWriterLoop();
}

void AUDIO_RS::NetworkTaskLoopTrampoline(void* pv)
{
    AUDIO_RS* self = static_cast<AUDIO_RS*>(pv);
    if (!self)
    {
        vTaskDelete(nullptr);
        return;
    }
    self->NetworkTaskLoop();
}

void AUDIO_RS::NetworkDataWriterLoopTrampoline(void* pv)
{
    AUDIO_RS* self = static_cast<AUDIO_RS*>(pv);
    if (!self)
    {
        vTaskDelete(nullptr);
        return;
    }
    self->NetworkDataWriterLoop();
}


void AUDIO_RS::nvsInitMain()
{
    esp_err_t nvs_err = nvs_flash_init();
    if (nvs_err == ESP_ERR_NVS_NO_FREE_PAGES || nvs_err == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
        Serial.println("AUDIO_RS::nvsInitMain:: Erasing & re-init");
        nvs_flash_erase();
        nvs_err = nvs_flash_init();
    }

    if (nvs_err != ESP_OK)
    {
        Serial.printf("AUDIO_RS::nvsInitMain::NVS init failed; %d\n", (int)nvs_err);
    }
    else
    {
        Serial.printf("AUDIO_RS::nvsInitMain::Passed");
    }
}