#include "headers/audio_read_send.h"
#include <esp_timer.h>
#include "driver/i2s.h"
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