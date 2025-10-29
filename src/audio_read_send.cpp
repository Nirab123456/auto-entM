#include "audio_read_send.h"
#include <esp_timer.h>
#include "driver/i2s.h"
#include "a_c_s.h"


AUDIO_RS::AUDIO_RS(
    std::span<uint32_t> i2s_buffer,
    std::span<uint32_t> ring_payload_flat,
    size_t frames_per_packet,
    std::shared_ptr<std::atomic<bool>> consumer_ready,
    std::shared_ptr<std::atomic<size_t>> ring_head,
    std::shared_ptr<std::atomic<size_t>> ring_tail,
    std::shared_ptr<std::atomic<uint64_t>> abs_idx
):
    i2s_buffer_(i2s_buffer),
    ring_payload_flat_(ring_payload_flat),
    frames_per_packet_(frames_per_packet),
    consumer_ready_sp_(std::move(consumer_ready)),
    ring_head_sp_(std::move(ring_head)),
    ring_tail_sp_(std::move(ring_tail)),
    abs_idx_sp_(std::move(abs_idx))
{}


void AUDIO_RS::set_consumer_ready(std::shared_ptr<std::atomic<bool>> ar)
{
    consumer_ready_sp_ = std::move(ar);
}
void AUDIO_RS::set_ring_head(std::shared_ptr<std::atomic<size_t>> ar)
{
    ring_head_sp_ = std::move(ar);
}
void AUDIO_RS::set_ring_tail(std::shared_ptr<std::atomic<size_t>> ar)
{
    ring_tail_sp_ = std::move(ar);
}
void AUDIO_RS::set_abs_idx(std::shared_ptr<std::atomic<uint64_t>> ar)
{
    abs_idx_sp_ = std::move(ar);
}
void AUDIO_RS::set_i2s_buffer(std::span<uint32_t>i2s_buffer)
{
    i2s_buffer_ = i2s_buffer;
}
void AUDIO_RS::set_ring_payload_flat(std::span<uint32_t> flat, size_t frames_per_packet)
{
    ring_payload_flat_ = flat;
    frames_per_packet_ = frames_per_packet;
}

void AUDIO_RS::AudioTaskTrampoline(void* pv)
{
    AUDIO_RS* self = static_cast<AUDIO_RS*>(pv);
    if (!self)
    {
        vTaskDelete(nullptr);
        return;
    }
    self ->AudioTaskLoop();
}

bool AUDIO_RS::start_task(const char* name,
                          uint32_t stack,
                          UBaseType_t prio,
                          BaseType_t core,
                          TASK_TRAMPOLINE_FN trampoline,
                          void* arg)
{
    // choose pv to pass into task
    void* pv_arg = (arg != nullptr) ? arg : this;

    BaseType_t ok;
    if (core >= 0) {
        ok = xTaskCreatePinnedToCore(
            trampoline,   // use provided trampoline
            name,
            stack,
            pv_arg,       // pv parameter forwarded
            prio,
            nullptr,
            core
        );
    } else {
        ok = xTaskCreate(
            trampoline,
            name,
            stack,
            pv_arg,
            prio,
            nullptr
        );
    }
    return (ok == pdPASS);
}

void AUDIO_RS::AudioTaskLoop()
{
    if (i2s_buffer_.size()== 0 || ring_payload_flat_.size()== 0 || frames_per_packet_ == 0)
    {
        vTaskDelay(pdMS_TO_TICKS(100));
        vTaskDelete(nullptr);
        return;
    }

    if (ring_payload_flat_.size() % frames_per_packet_ != 0)
    {
        Serial.println("AUDIO_RS::AudioTaskLoop - ring payload size not divisible by frames_per_packet");
        vTaskDelay(pdMS_TO_TICKS(100));
        vTaskDelete(nullptr);
        return;
    }
    const size_t ring_slots = ring_payload_flat_.size() / frames_per_packet_;
    const size_t frames     = frames_per_packet_;
    
    Serial.printf("TASK : AUDIOTASK started, frames=%u ring_slots=%u\n", (unsigned)frames, (unsigned)ring_slots);
    bool paused = false;
    for (;;)
    {
        if (consumer_ready_sp_)
        {
            if (!consumer_ready_sp_ -> load(std::memory_order_acquire))
            {
                if (!paused)
                {
                    Serial.println("AUDIO_RS::AudioTaskLoop - No receiver connected, pausing");
                    i2s_zero_dma_buffer(I2S_NUM_0);
                    paused = true;
                }
                vTaskDelay(pdMS_TO_TICKS(200));
                continue;
            }
            else
            {
                if (paused)
                {
                    Serial.println("AUDIO_RS::AudioTaskLoop -> Resuming");
                    paused = false;
                }
                
            }  
        }

        size_t bytes_read = 0;
        esp_err_t err = i2s_read(
            I2S_NUM_0,
            i2s_buffer_.data(),
            i2s_buffer_.size() * sizeof(uint32_t),
            &bytes_read,
            portMAX_DELAY
        );

        if (err != ESP_OK || bytes_read == 0)
        {
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        size_t word_count = bytes_read /sizeof(uint32_t);
        size_t available_frames = (word_count >= (frames * DEFAULT_CHANNEL_COUNT)) ? frames : (word_count / DEFAULT_CHANNEL_COUNT);

        if (available_frames > frames)
        {
            available_frames = frames;
        }

        size_t head = ring_head_sp_ ? ring_head_sp_ -> load(std::memory_order_relaxed) : 0;
        size_t tail = ring_tail_sp_ ? ring_tail_sp_ -> load(std::memory_order_acquire) : 0;
        size_t next_head = head + 1;


        if ((next_head - tail) > ring_slots)
        {
            if (abs_idx_sp_)
            {
                abs_idx_sp_ -> fetch_add(
                    (uint64_t)available_frames,
                    std::memory_order_relaxed
                );
                static unsigned drop_count = 0;
                if ((++drop_count % 10)== 0)
                {
                    Serial.printf("AUDIO RING: BUFFER FULL, head-tail=%u free heap=%u\n", (unsigned)(head - tail), (unsigned)esp_get_free_heap_size());
                }
            }    
            taskYIELD();
            continue;
        }
        
        size_t slot = head & (ring_slots -1);
        uint32_t* row_ptr = ring_payload_flat_.data() + slot * frames;
        std::span<uint32_t> row(row_ptr,frames);



        if (available_frames == frames)
        {
            for (size_t i = 0; i < frames; i++)
            {
                row[i] = i2s_buffer_[i * DEFAULT_CHANNEL_COUNT + 1];
            }
        }
        else
        {
            for (size_t i = 0; i < available_frames; i++)
            {
                row[i] = i2s_buffer_[i * DEFAULT_CHANNEL_COUNT + 1];
            }
            
            for (size_t i = available_frames; i < frames; i++)
            {
                row[i] = 0;
            }
        }

        uint64_t first_sample_idx =  abs_idx_sp_ ? abs_idx_sp_ ->load(std::memory_order_relaxed) : 0;
        uint64_t ts               = (uint64_t)esp_timer_get_time();

        if (ring_head_sp_)
        {
            ring_head_sp_ ->store(next_head, std::memory_order_release);
    
        }
        if(abs_idx_sp_)
        {
            abs_idx_sp_ ->fetch_add(
                (uint64_t)available_frames,
                std::memory_order_relaxed
            );
        }    
        taskYIELD();
    }
        
}

void AUDIO_RS::I2SReaderLoop()
{
    if (i2s_buffer_.size() == 0 || i2s_queue_ == nullptr) {
        vTaskDelay(pdMS_TO_TICKS(100));
        vTaskDelete(nullptr);
        return;
    }

    for (;;) {
        size_t bytes_read = 0;
        esp_err_t err = i2s_read(
            I2S_NUM_0,
            i2s_buffer_.data(),
            i2s_buffer_.size() * sizeof(uint32_t),
            &bytes_read,
            portMAX_DELAY
        );

        if (err != ESP_OK || bytes_read == 0) {
            // small sleep and retry
            vTaskDelay(pdMS_TO_TICKS(5));
            continue;
        }

        // push to queue (if full, drop oldest by overwriting: try send with timeout 0)
        if (xQueueSend(i2s_queue_, &bytes_read, 0) != pdTRUE) {
            // queue full -> try overwrite by receiving one and sending again (simple discard policy)
            size_t dummy;
            xQueueReceive(i2s_queue_, &dummy, 0);
            xQueueSend(i2s_queue_, &bytes_read, 0);
        }
    }
}


void AUDIO_RS::RingWriterLoop()
{
    if (i2s_buffer_.size() == 0 || ring_payload_flat_.size() == 0 || frames_per_packet == 0 || i2s_queue_ == nullptr)
    {
        vTaskDelay(pdMS_TO_TICKS(100));
        vTaskDelete(nullptr);
        return;
    }
    if (ring_payload_flat_.size() % frames_per_packet_ != 0)
    {
        Serial.println("RingWriterLoop - ring size not multiple of frames_per_packet");
        vTaskDelay(pdMS_TO_TICKS(100));
        vTaskDelete(nullptr);
        return;     
    }

    const size_t ring_slots = ring_payload_flat_.size() / frames_per_packet_;
    const size_t frames = frames_per_packet_;
    const bool ring_power_of_two = (ring_slots & (ring_slots - 1)) == 0;
    const size_t ring_mask = ring_power_of_two ? (ring_slots - 1) : 0;
    
    for (;;)
    {
        size_t bytes_read = 0;
        if (xQueueReceive(i2s_queue_,&bytes_read,portMAX_DELAY) != pdTRUE)
        {
            continue;
        }
        size_t word_count = bytes_read / sizeof(uint32_t);
        size_t available_frames = 0;
        if (word_count >= (frames * DEFAULT_CHANNEL_COUNT))
        {
            available_frames = frames;
        }
        else
        {
            available_frames = (word_count / DEFAULT_CHANNEL_COUNT);
        }
        if ((available_frames > frames))
        {
            available_frames = frames;
        }
        size_t head = ring_head_sp_ ? ring_head_sp_ -> load(std::memory_order_relaxed) : 0;
        size_t tail = ring_tail_sp_ ? ring_tail_sp_ -> load(std::memory_order_acquire) : 0;
        size_t next_head = head + 1;

        if ((next_head -tail) > ring_slots)
        {
            if (overrun_policy_ == OverRunPolicy::DROP_NEWEST)
            {
                drop_count_newest_.fetch_add(1,std::memory_order_relaxed);
                if (abs_idx_sp_)
                {
                    abs_idx_sp_ -> fetch_add(
                        (uint64_t)available_frames,
                        std::memory_order_relaxed
                    );
                }
                taskYIELD();
                continue;
            }
            else
            {
                if (ring_tail_sp_)
                {
                    ring_tail_sp_ -> fetch_add(
                        1,
                        std::memory_order_acq_rel
                    );
                    drop_count_oldest_.fetch_add(
                        1,
                        std::memory_order_relaxed
                    );
                    tail = ring_tail_sp_ ->load(std::memory_order_acquire);
                }
                else
                {
                    drop_count_newest_.fetch_add(
                        1,
                        std::memory_order_relaxed
                    );
                    if (abs_idx_sp_)
                    {
                        abs_idx_sp_->fetch_add(
                            (uint64_t)available_frames,
                            std::memory_order_relaxed
                        );
                    }
                    taskYIELD();
                    continue;
                }
            }
        }
        size_t slot = ring_power_of_two ? (head & ring_mask) : (head % ring_slots);
        uint32_t* row_ptr = ring_payload_flat_.data() + slot * frames;
        std::span<uint32_t> row(row_ptr, frames);
        if (available_frames == frames)
        {
            for (size_t i = 0; i < frames; i++)
            {
                row[i] = i2s_buffer_[i * DEFAULT_CHANNEL_COUNT];
            }
            for (size_t i = available_frames; i < frames; i++)
            {
                row[i] = 0;
            }
        }
        
        //meta
        uint64_t first_sample_idx = abs_idx_sp_ ? abs_idx_sp_->load(std::memory_order_release) : 0;
        uint64_t ts = (uint64_t)esp_timer_get_time();

        if (ring_head_sp_)
        {
            ring_head_sp_->store(next_head,std::memory_order_release);
        }
        if (abs_idx_sp_)
        {
            abs_idx_sp_->fetch_add(
                (uint64_t) available_frames,
                std::memory_order_relaxed
            );
        }
        
    }
    
}