#include "headers/audio_read_send.h"
#include <esp_timer.h>
#include "driver/i2s_std.h"
#include "headers/a_c_s.h"
#include "headers/ReciverConfig.h"   // <<--- add this (exact filename may differ)



bool AUDIO_RS::initI2S()
{
    if (!mic_configured_.load(std::memory_order_acquire))
    {
        Serial.println("AUDIO_RS::initI2S::Mic not configured");
    }
    if (frames_per_packet_ == 0)
    {
        Serial.println("AUDIO_RS::initI2S::Frames per packet = 0");
    }

    if (i2s_installed_.load(std::memory_order_acquire))
    {
        if (rx_chan_)
        {
            i2s_channel_disable(rx_chan_);
            i2s_del_channel(rx_chan_);
            rx_chan_ = nullptr;

        }
        if (tx_chan_)
        {
            i2s_channel_disable(tx_chan_);
            i2s_del_channel(tx_chan_);
            tx_chan_ = nullptr;
        }
        i2s_installed_ = false;
    }
    size_t dma_frame_len = frames_per_packet_ / DEFAULT_CHANNEL_COUNT;
    if (dma_frame_len <  BYTES_PER_SAMPLE)
    {
        dma_frame_len = BYTES_PER_SAMPLE;
    }
    if (dma_frame_len > 2048)
    {
        dma_frame_len = 2048;
    }

    int dma_frame_num = 6;

    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(micfg_.i2s_port, I2S_ROLE_MASTER);
    
    chan_cfg.dma_frame_num = dma_frame_num;
    chan_cfg.dma_desc_num = (int)dma_frame_len;

    esp_err_t err = i2s_new_channel(&chan_cfg, nullptr, &rx_chan_);
    if (err != ESP_OK || rx_chan_ == nullptr)
    {
        Serial.printf("AUDIO_RS::initI2s->i2s_new_channel():: Creation failed with err->%d\n",(int)err);
        rx_chan_ = nullptr;
        return false;
    }

    err = i2s_channel_init_std_mode(rx_chan_, &micfg_.i2s_configuration);

    if (err != ESP_OK)
    {
        Serial.printf("AUDIO_RS::initI2S-> i2s_channel_init_std_mode()::Failed =%d\n",(int)err);
        i2s_channel_disable(rx_chan_);
        i2s_del_channel(rx_chan_);
        rx_chan_ = nullptr;
        return false;
    }
    
    err = i2s_channel_enable(rx_chan_);

    if (err != ESP_OK)
    {
        Serial.printf("AUDIO_RS::initI2S->i2s_channel_enable():Channel enable failed =%d\n",(int)err);
        i2s_channel_disable(rx_chan_);
        i2s_del_channel(rx_chan_);
        rx_chan_ = nullptr;
        return false;

    }

    i2s_channel_disable(rx_chan_);
    vTaskDelay(pdMS_TO_TICKS(5));
    i2s_channel_enable(rx_chan_);

    if (i2s_queue_ == nullptr)
    {
        i2s_queue_ = xQueueCreate(SIZE_OF_A_BYTE_IN_BITS, sizeof(size_t));

        if (!i2s_queue_)
        {
            Serial.println("AUDIO_RS::initI2S->i2s_queue_::Autocreate failed");
        }
        else
        {
            Serial.println("AUDIO_RS::initI2S->i2s_queue_::Created");
        }
    }

    if (network_slot_queue_ == nullptr)
    {
        Serial.println("AUDIO_RS::initI2S ->network_slot_queue_:: nullptr");
    }
    
    i2s_installed_ = true;    
    Serial.printf("I2S: initialized (dma_frame_num=%d dma_frame_len=%u)\n",
                  dma_frame_num, (unsigned)dma_frame_len);
    return true;
}

void AUDIO_RS::deinitI2S()
{
    if (!i2s_installed_)
    {
        return;
    }
    if (rx_chan_)
    {
        i2s_channel_disable(rx_chan_);
        i2s_del_channel(rx_chan_);
        rx_chan_ = nullptr;
    }

    if (tx_chan_)
    {
        i2s_channel_disable(tx_chan_);
        i2s_del_channel(tx_chan_);
        tx_chan_ = nullptr;
    }
    
    i2s_installed_ = false;
    Serial.println("AUDIO_RS::deinitI2S::Driver uninstalled");
    
}


void AUDIO_RS::I2SReaderLoop()
{
    if (stopping_check_del("I2SReaderLoop"))
    {
        vTaskDelete(nullptr);
    }
    
    if (!i2s_installed_) {
        Serial.println("I2SReaderLoop: I2S driver not installed");
        vTaskDelay(pdMS_TO_TICKS(100));
        vTaskDelete(nullptr);
        return;
    }
    if (i2s_buffer_.size() == 0) {
        Serial.println("I2SReaderLoop: i2s buffer not configured");
        vTaskDelay(pdMS_TO_TICKS(100));
        vTaskDelete(nullptr);
        return;
    }
    if (i2s_queue_ == nullptr) {
        Serial.println("I2SReaderLoop: i2s_queue_ not set");
        vTaskDelay(pdMS_TO_TICKS(100));
        vTaskDelete(nullptr);
        return;
    }

    size_t bytes_to_read = i2s_buffer_.size() * sizeof(uint32_t);
    const TickType_t short_delay = pdMS_TO_TICKS(5);

    unsigned consecutive_read_failure = 0;

    for (;;)
    {
        if (stopping_.load(std::memory_order_acquire))
        {
            break;
        }
        
        if (ulTaskNotifyTake(pdTRUE, 0) > 0)
        {
            Serial.println("I2SReaderLoop:: Notified to exit");
            break;
        }

        size_t read_bytes = 0;
        esp_err_t err = i2s_channel_read(
            rx_chan_,
            i2s_buffer_.data(),
            bytes_to_read,
            &read_bytes,
            portMAX_DELAY
        );
        
        if (err != ESP_OK || read_bytes == 0)
        {
            ++consecutive_read_failure;
            if ((consecutive_read_failure & 0xff) == 0)
            {
                Serial.printf("I2SReaderLoop: read err=%d bytes=%d\n",(int)err, (unsigned)bytes_to_read);
            }
            vTaskDelay(short_delay);
            continue;
        }
        consecutive_read_failure = 0;
        BaseType_t sent = xQueueSend(i2s_queue_, &read_bytes, 0);
        if (sent != pdTRUE)
        {
            if (overrun_policy_ == OverRunPolicy::DROP_OLDEST)
            {
                size_t dropped;
                if (xQueueReceive(i2s_queue_, &dropped, 0) == pdTRUE)
                {
                    // we dropped 'dropped' (bytes count) older sample
                }
                if (xQueueSend(i2s_queue_, &read_bytes, 0) !=pdTRUE)
                {
                    drop_count_newest_.fetch_add(1,std::memory_order_relaxed);
                }  
            }
            else if (overrun_policy_ == OverRunPolicy::DROP_NEWEST)
            {
                drop_count_newest_.fetch_add(1,std::memory_order_relaxed);
            }   
        }
        
        taskYIELD();
    }
    Serial.println("I2SReaderLoop:: Exiting");
    vTaskDelete(nullptr);    
}


void AUDIO_RS::RingWriterLoop()
{
    if (stopping_check_del("RingWriterLoop"))
    {
        vTaskDelete(nullptr);
    }
    
    if (i2s_buffer_.size() == 0 || ring_payload_flat_.size() == 0 || frames_per_packet_ == 0 || i2s_queue_ == nullptr)
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
    const size_t channel_index = (DEFAULT_CHANNEL_COUNT > 1) ? 1u : 0u;
    
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

        size_t i = 0;
        for (; i < available_frames && i < frames; i++)
        {
            size_t sample_word_index = (i * DEFAULT_CHANNEL_COUNT) + channel_index;
            if (sample_word_index < i2s_buffer_.size())
            {
                row[i] = i2s_buffer_[sample_word_index];
            }
            else
            {
                row[i] = 0u;
            }
        }
        for (; i < frames; i++)
        {
            row[i] = 0u;
        }
        
        uint64_t first_sample_idx = abs_idx_sp_ ? abs_idx_sp_->load(std::memory_order_relaxed) : 0;
        uint64_t ts = (uint64_t)esp_timer_get_time();
        if (ring_frames_span_.size() == ring_slots)
        {
            ring_frames_span_[slot] = static_cast<uint16_t>(available_frames);
        }
        if (ring_first_index_span_.size()==ring_slots)
        {
            ring_first_index_span_[slot] = first_sample_idx;
        }
        if (ring_timestamp_span_.size()==ring_slots)
        {
            ring_timestamp_span_[slot] = ts;
        }
        if (ring_head_sp_)
        {
            ring_head_sp_->store(next_head, std::memory_order_release);
        }
        if (abs_idx_sp_)
        {
            abs_idx_sp_->fetch_add((uint64_t)available_frames, std::memory_order_relaxed);
        }
        if (network_slot_queue_)
        {
            size_t s = slot;
            if (xQueueSend(network_slot_queue_, &s, 0) != pdTRUE)
            {
                /* code */
            }
            
        }
        taskYIELD();   
    }
    
}
