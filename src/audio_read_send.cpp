#include "audio_read_send.h"

AUDIO_RS::AUDIO_RS()
    :
    Consumer_Ready_SP_(nullptr),
    buffer_ptr_(nullptr),
    buffer_len_(0)
{}

AUDIO_RS::AUDIO_RS(uint32_t* buffer, size_t length)
    :
    Consumer_Ready_SP_(nullptr),
    buffer_ptr_(buffer),
    buffer_len_(length)
{}

AUDIO_RS::AUDIO_RS(std::shared_ptr<std::atomic<bool>> SP_consumer_ready)
    :
    Consumer_Ready_SP_(std::move(SP_consumer_ready)),
    buffer_ptr_(nullptr),
    buffer_len_(0)
{}

AUDIO_RS::AUDIO_RS(uint32_t* buffer,
     size_t leangth,
     std::shared_ptr<std::atomic<bool>> SP_consumer_ready
    )
    :
    Consumer_Ready_SP_(std::move(SP_consumer_ready)),
    buffer_ptr_(buffer),
    buffer_len_(leangth)
{}

uint32_t* AUDIO_RS::buffer()    const
{
    return buffer_ptr_;
}
size_t AUDIO_RS::buffer_len()   const
{
    return buffer_len_;
}
bool AUDIO_RS::has_consumer_ready() const
{
    return static_cast<bool>(Consumer_Ready_SP_);
}
void AUDIO_RS:: Audio_Task(void*pv)
{
    if (!buffer_ptr_ || buffer_len_)
    {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    bool ready = false;
    if (Consumer_Ready_SP_)
    {
        ready = Consumer_Ready_SP_ -> load(std::memory_order_acquire);
    }
    bool pauseD = false;
    while (true)
    {
        if (ready)
        {
            if (!pauseD)
            {
                Serial.println("AUDIO_TASK:: NO reciver connected");
                i2s_zero_dma_buffer(I2S_NUM_0);
                vTaskDelay(pdMS_TO_TICKS(200));
                continue;
            }
            else
            {
                if (pauseD)
                {
                    Serial.println("AUDIO_TASK:: Resuming");
                    pauseD = false;
                }

            }
        }
        
    }
    
    

}