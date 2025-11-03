#include "headers/audio_read_send.h"
#include <esp_timer.h>
#include "driver/i2s.h"
#include "headers/a_c_s.h"
#include "headers/ReciverConfig.h"   // <<--- add this (exact filename may differ)


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

void AUDIO_RS::Ring_clear_Rst()
{
    if (ring_payload_flat_.size() == 0 &&
        i2s_buffer_.size() == 0 &&
        !ring_head_sp_ &&
        !ring_tail_sp_&&
        !abs_idx_sp_
        )       
    {
        return;
    }

    vTaskSuspendAll();
    if (ring_payload_flat_.size() > 0)
    {
        std::fill(ring_payload_flat_.begin(),ring_payload_flat_.end(),0u);
    }
    if (i2s_buffer_.size() > 0)
    {
        std::fill(i2s_buffer_.begin(),i2s_buffer_.end(),0u);
    }
    
    if (ring_tail_sp_)
    {
        ring_tail_sp_->store(0,std::memory_order_relaxed);
    }
    if (ring_head_sp_)
    {
        ring_head_sp_->store(0,std::memory_order_relaxed);
    }
    if (abs_idx_sp_)
    {
        abs_idx_sp_->store(0,std::memory_order_relaxed);
    }
    drop_count_newest_.store(0u, std::memory_order_relaxed);
    drop_count_oldest_.store(0u, std::memory_order_relaxed);
    
    const size_t ring_slots = (frames_per_packet_ > 0) ? (ring_payload_flat_.size() / frames_per_packet_) : 0;
    if (ring_slots > 0)
    {
        if (ring_frames_span_.size() == ring_slots)
        {
            std::fill(ring_frames_span_.begin(), ring_frames_span_.end(), 0u);
        }
        if (ring_first_index_span_.size() == ring_slots)
        {
            std::fill(ring_first_index_span_.begin(), ring_first_index_span_.end(), 0u);
        }
        if (ring_timestamp_span_.size() == ring_slots)
        {
            std::fill(ring_timestamp_span_.begin(), ring_timestamp_span_.end(), 0u);
        }
        
        
    }
    
    xTaskResumeAll();
    
}
void AUDIO_RS::NetworkTaskLoop()
{
    if (ring_payload_flat_.size() == 0 || frames_per_packet_ == 0)
    {
        Serial.println("NetworkLoop: ring or frames not configured");
        vTaskDelay(pdMS_TO_TICKS(100));
        vTaskDelete(nullptr);
        return;
    }

    const size_t ring_slots = ring_payload_flat_.size() /frames_per_packet_;
    if (ring_slots == 0)
    {
        Serial.println("NetworkTaskLoop : invalid ring_slots");
        vTaskDelay(pdMS_TO_TICKS(100));
        vTaskDelete(nullptr);
        return;
    }
    
    unsigned long last_conn_attempt = 0;

    for (;;)
    {
        bool have_cfg = (recfg_ptr_ && recfg_ptr_->isValid());
        bool connected = (WiFi_tcp_client_ptr_ && WiFi_tcp_client_ptr_->connected());
        unsigned long now = millis();

        if (!connected)
        {
            if (have_cfg && WiFi.isConnected())
            {
                if ((now - last_conn_attempt) >= CONNECTION_RETRY_INTERVAL_MS)
                {
                    last_conn_attempt = now;
                    if (consumer_ready_sp_)
                    {
                        consumer_ready_sp_->store(false, std::memory_order_release);
                    }
                    Ring_clear_Rst();
                    bool ok = false;
                    if (WiFi_tcp_client_ptr_ != nullptr)
                    {
                        ok = recfg_ptr_->ConnectTOReciverIP(WiFi_tcp_client_ptr_);
                    }
                    else
                    {
                        //backbone for cross communication 
                        // ok = tcp_connect_fn_(remote_ip,remote_port);
                    }

                    if (ok)
                    {
                        if (consumer_ready_sp_)
                        {
                            consumer_ready_sp_->store(true, std::memory_order_release);
                        }
                    }
                    else
                    {
                        //failed to connect
                        vTaskDelay(pdMS_TO_TICKS(20));
                        continue;
                    }
                }
                
            }
            
        }

        if (!have_cfg && !WiFi.isConnected())
        {
            vTaskDelay(pdMS_TO_TICKS(200));
            continue;
        }

        
        size_t tail = ring_tail_sp_ ? ring_tail_sp_->load(std::memory_order_acquire) : 0;
        size_t head = ring_head_sp_ ? ring_head_sp_->load(std::memory_order_acquire) : 0;
        if (tail ==head)
        {
            vTaskDelay(pdMS_TO_TICKS(2));
            continue;
        }
        size_t slot = tail & (ring_slots -1);

        if (ring_frames_span_.size() != ring_slots)
        {
            Serial.println("NetworkTaskLoop: ring_frames_span_ not configured");
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }
        


        uint16_t frames = ring_frames_span_[slot];
        if (frames == 0 || frames > static_cast<uint16_t>(frames_per_packet_))
        {
            if (ring_tail_sp_)
            {
                ring_tail_sp_->store(tail + 1, std::memory_order_release);
            }
            continue;            
        }

        if (network_slot_queue_)
        {
            size_t s = slot;
            if (xQueueSend(network_slot_queue_, &s, 0) != pdTRUE)
            {
                //queue full
            }
            else
            {
                //sussess
            }
            
            
        }
        vTaskDelay(pdMS_TO_TICKS(1));
    }
}

void AUDIO_RS::NetworkDataWriterLoop()
{
    if (!network_slot_queue_) {
        vTaskDelay(pdMS_TO_TICKS(100));
        vTaskDelete(nullptr);
        return;
    }

    if (frames_per_packet_ == 0 || ring_payload_flat_.size() == 0)
    {
        vTaskDelay(pdMS_TO_TICKS(100));
        vTaskDelete(nullptr);
        return;
    }
    const size_t ring_slots = ring_payload_flat_.size() / frames_per_packet_;
    if (ring_slots == 0)
    {
        vTaskDelay(pdMS_TO_TICKS(100));
        vTaskDelete(nullptr);
        return;
    }


    for (;;)
    {
        size_t slot = 0;
        if (xQueueReceive(network_slot_queue_, &slot, portMAX_DELAY) != pdTRUE)
        {
            continue;
        }
        if (slot >= ring_slots)
        {
            continue;
        }
        if (
            ring_frames_span_.size() != ring_slots ||
            ring_first_index_span_.size() != ring_slots||
            ring_timestamp_span_.size() != ring_slots
        )
        {
            size_t tail = ring_tail_sp_ ? ring_tail_sp_->load(std::memory_order_acquire) : 0;
            if (ring_tail_sp_)
            {
                ring_tail_sp_->store(tail + 1, std::memory_order_release);
            }
            continue;
        }
        
        const uint16_t frames = ring_frames_span_[slot];
        const uint64_t first_index = ring_first_index_span_[slot];
        const uint64_t ts = ring_timestamp_span_[slot];

        if (frames == 0 || frames > static_cast<uint16_t>(frames_per_packet_))
        {
            size_t tail =  0;
            if (ring_tail_sp_)
            {
                tail = ring_tail_sp_->load(std::memory_order_acquire);
                ring_tail_sp_->store(tail + 1, std::memory_order_release);
            }
            continue;
        }

        uint32_t seq = sequence_counter_ ? sequence_counter_->fetch_add(1, std::memory_order_relaxed) : 0;

        if (write_tcp_header_fn_)
        {
            //backbone for cross communication
            write_tcp_header_fn_(seq, first_index, ts, frames);
        }
        else
        {
            WriteTCPHeader(seq, first_index, ts, frames);
        }

        uint8_t* header_ptr = header_buffer_.data();
        size_t header_len = header_size_;

        uint32_t* row_ptr = ring_payload_flat_.data() + slot * frames_per_packet_;
        uint8_t* payload_ptr = reinterpret_cast<uint8_t*>(row_ptr);
        size_t payload_len = static_cast<size_t>(frames) * sizeof(uint32_t);

        bool success = false;

        if (!WiFi_tcp_client_ptr_ || !WiFi_tcp_client_ptr_->connected())
        {
            if (recfg_ptr_ && WiFi_tcp_client_ptr_)
            {
                if(!recfg_ptr_->ConnectTOReciverIP(WiFi_tcp_client_ptr_))
                {
                    xQueueSend(network_slot_queue_, &slot, 0);
                    vTaskDelay(pdMS_TO_TICKS(50));
                    continue;
                }
            }
            else
            {
                xQueueSendToBack(network_slot_queue_, &slot, 0);
                vTaskDelay(pdMS_TO_TICKS(50));
                continue;
            }
        }

        bool write_ok = false;
        if (recfg_ptr_)
        {
            //header
            write_ok = recfg_ptr_->TCPWriteAll(WiFi_tcp_client_ptr_, header_ptr, payload_len, 4000, 3, 1400);
            success = write_ok;
            if (success)
            {
                //payload                
                write_ok = recfg_ptr_->TCPWriteAll(WiFi_tcp_client_ptr_, payload_ptr, payload_len, 4000, 3, 1400);
                success = write_ok;
            }
        }
        else
        {
            //fallback if needed 
        }

        if (!success)
        {
            if (tcp_client_stop_fn_)
            {
                tcp_client_stop_fn_();
            }
            if (WiFi_tcp_client_ptr_)
            {
                WiFi_tcp_client_ptr_->stop();
            }
            if (consumer_ready_sp_)
            {
                consumer_ready_sp_->store(false, std::memory_order_release);
            }
            Ring_clear_Rst();
            xQueueSendToBack(network_slot_queue_, &slot, 0);
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }
        
        if (ring_tail_sp_)
        {
            size_t tail = ring_tail_sp_->load(std::memory_order_acquire);
            ring_tail_sp_->store(tail+1, std::memory_order_release);
        }
        taskYIELD();
    }
    
}
