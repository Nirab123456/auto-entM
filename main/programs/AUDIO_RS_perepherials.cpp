#include "headers/audio_read_send.h"
#include <esp_timer.h>
#include "driver/i2s_std.h"
#include "headers/a_c_s.h"
#include "headers/ReciverConfig.h"   // <<--- add this (exact filename may differ)


static const char *phpTAG = "AUDIO_RS_perepherials";


bool AUDIO_RS::BasicNecesseryChecksLoop(char* taskname)
{
    if (stopping_check_del(taskname))
    {
        return false;
    }
    if (!i2s_installed_) {
        ESP_LOGE(phpTAG, "AUDIO_RS-> %s-> BasicNecesseryChecksLoop(char* taskname): I2S driver not installed", taskname);
        return false;
    }
    if (i2s_buffer_.size() == 0) {
        ESP_LOGE(phpTAG, "AUDIO_RS-> %s-> BasicNecesseryChecksLoop(char* taskname): i2s buffer not configured", taskname);
        return false;
    }
    if (i2s_queue_ == nullptr) {
        ESP_LOGE(phpTAG, "AUDIO_RS-> %s-> BasicNecesseryChecksLoop(char* taskname): i2s_queue_ not set", taskname);
        return false;
    }

        
    if (strcmp(taskname, "RingWriterLoop") == 0)
    {
        if (ring_payload_flat_.size() == 0)
        {
            ESP_LOGE(phpTAG, "AUDIO_RS-> %s-> BasicNecesseryChecksLoop(char* taskname):ring_payload_flat_.size() = 0");
            return false;
        }
        if (frames_per_packet_ == 0)
        {
            ESP_LOGE(phpTAG, "AUDIO_RS-> %s-> BasicNecesseryChecksLoop(char* taskname):frames_per_packet_ = 0");
            return false;
        }
        if (ring_payload_flat_.size() % frames_per_packet_ != 0)
        {
            ESP_LOGE(phpTAG, "AUDIO_RS-> %s-> BasicNecesseryChecksLoop(char* taskname):ring_payload_flat_.size() is not a multiple of frames_per_packet_");
            return false;
        }
    }
    

    return true;
}


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
        ESP_LOGI(phpTAG, "AUDIO_RS::nvsInitMain:: Erasing & re-init");
        nvs_flash_erase();
        nvs_err = nvs_flash_init();
    }

    if (nvs_err != ESP_OK)
    {
        ESP_LOGE(phpTAG, "AUDIO_RS::nvsInitMain::NVS init failed; %d\n", (int)nvs_err);
    }
    else
    {
        ESP_LOGI(phpTAG, "AUDIO_RS::nvsInitMain::Passed");
    }
}