#include "audio_read_send.h"
#include "audio_conf_sett.h"
//default ctor
AUDIO_RS::AUDIO_RS() = default;


//buffer-only
AUDIO_RS::AUDIO_RS(uint32_t* buffer, size_t length)
    : buffer_ptr_(buffer), buffer_len_(length)
{}

//full
AUDIO_RS::AUDIO_RS(
    uint32_t* buffer,
    size_t length,
    std::shared_ptr<std::atomic<bool>>      consumer_ready,
    std::shared_ptr<std::atomic<size_t>>    ring_head,
    std::shared_ptr<std::atomic<size_t>>    ring_tail


):
    buffer_ptr_(buffer),
    buffer_len_(length),
    Consumer_ready_ar_(std::move(consumer_ready)),
    Ring_head_ar_(std::move(ring_head)),
    Ring_tail_ar_(std::move(ring_tail))

{}

void AUDIO_RS::set_consumer_ready(std::shared_ptr<std::atomic<bool>> ar)
{
    Consumer_ready_ar_ = std::move(ar);
}

void AUDIO_RS::set_ring_head(std::shared_ptr<std::atomic<size_t>>ar)
{
    Ring_head_ar_ = std::move(ar);
}

void::AUDIO_RS::set_ring_tail(std::shared_ptr<std::atomic<size_t>>ar)
{
    Ring_tail_ar_ = std::move(ar);
}








uint32_t* AUDIO_RS::buffer() const
{
    return buffer_ptr_;
}

size_t AUDIO_RS::buffer_len() const
{
    return buffer_len_;
}

bool AUDIO_RS::has_consumer_ready() const
{
    return static_cast<bool>(Consumer_ready_ar_);
}

void AUDIO_RS::Audio_Task(void* pv)
{
    if (!buffer_ptr_ || buffer_len == 0)
    {
        vTaskDelay(pdMS_TO_TICKS(10));
        return;
    }

    if (!Ring_head_ar_ || !Ring_tail_ar_)
    {
        return;
    }
    
    
    
}