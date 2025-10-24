#include "audio_read_send.h"
#include "audio_conf_sett.h"
//default ctor
AUDIO_RS::AUDIO_RS() = default;


//i2s_word_buffer-only
AUDIO_RS::AUDIO_RS(uint32_t* i2s_word_buffer, size_t length)
    : i2s_word_buffer_ptr_(i2s_word_buffer), buffer_len_(length)
{}

//full
AUDIO_RS::AUDIO_RS(
    uint32_t* i2s_word_buffer,
    size_t length,
    std::shared_ptr<std::atomic<bool>>      consumer_ready,
    std::shared_ptr<std::atomic<size_t>>    ring_head,
    std::shared_ptr<std::atomic<size_t>>    ring_tail,
    std::shared_ptr<std::atomic<uint64_t>>  absolute_sample_idx


):
    i2s_word_buffer_ptr_(i2s_word_buffer),
    buffer_len_(length),
    Consumer_ready_ar_(std::move(consumer_ready)),
    Ring_head_ar_(std::move(ring_head)),
    Ring_tail_ar_(std::move(ring_tail)),
    Abs_Idx_(std::move(absolute_sample_idx))

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

void::AUDIO_RS::set_abs_idx(std::shared_ptr<std::atomic<uint64_t>>ar)
{
    Abs_Idx_ = std::move(ar);
}






uint32_t* AUDIO_RS::i2s_word_buffer() const
{
    return i2s_word_buffer_ptr_;
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
    if (!i2s_word_buffer_ptr_ || buffer_len == 0)
    {
        vTaskDelay(pdMS_TO_TICKS(10));
        return;
    }

    if (!Ring_head_ar_ || !Ring_tail_ar_)
    {
        return;
    }
    if (!Consumer_ready_ar_)
    {
        return;
    }

    (void)pv;
    Serial.printf("TASK : AUDIOTASK \nFrames : %u Bytes to read : %u Payload : %u \n",FRAMES_PER_PACKET,(unsigned)BYTES_TO_READ,(unsigned)PAYLOAD_BYTES);
    bool paused = false;
    while (true)
    {
        if (Consumer_ready_ar_ ->load(std::memory_order_acquire))
        {
            if (!paused)
            {
                Serial.println("AUDIO_RS::audio_task:: No reciver connected");
                i2s_zero_dma_buffer(I2S_NUM_0);
                vTaskDelay(pdMS_TO_TICKS(200));
                continue;
            }
        }
        else
        {
            if (paused)
            {
                Serial.println("AUDIO_RS::audio_task:: Resuming");
                paused = false;
            }
        }
        size_t already_read_bytes = 0;
        esp_err_t i2s_read_error = i2s_read(
            I2S_NUM_0,
            (void*) i2s_word_buffer_ptr_,
            BYTES_TO_READ,
            &already_read_bytes,
            portMAX_DELAY
        );
        size_t word_count = already_read_bytes/BYTES_PER_SAMPLE;
        size_t available_frames = (
            (word_count >= NEEDED_WORDS) ? FRAMES_PER_PACKET : (word_count/DEFAULT_CHANNEL_COUNT)
        );

        if (available_frames > FRAMES_PER_PACKET)
        {
            available_frames = FRAMES_PER_PACKET;
        }
        size_t head = Ring_head_ar_ ->load(std::memory_order_relaxed);
        size_t tail = Ring_tail_ar_ ->load(std::memory_order_acquire);
        size_t next_head = head +1;
        if ((next_head - tail)>RING_SIZE)
        {        
            Abs_Idx_ -> fetch_add(
                (uint64_t) available_frames,
                std::memory_order_relaxed
            );
            static unsigned drop_count = 0 ;
           if ((++drop_count%10)==0)
           {
                Serial.printf("AUDIO RING : BUFFER FULL- Amont of packet dropping : %u\nAvailable free heap = %u\n",(unsigned)(head-tail),(unsigned)esp_get_free_heap_size());           
           }
           taskYIELD();
           continue; 
        }
        size_t slot = head & (RING_SIZE-1); //RING_MASK = (RING_SIZE-1)
        if (available_frames == FRAMES_PER_PACKET)
        {
            for (size_t i = 0; i < FRAMES_PER_PACKET; i++)
            {

            }
            
        }
        
        
        
    }
    
    
    
    
}