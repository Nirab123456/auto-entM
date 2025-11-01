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
    for (size_t i = 0; i < ring_payload_flat_.size() / frames_per_packet_; i++)
    {
        break;
    }
    xTaskResumeAll();
    
}
void AUDIO_RS::NetworkTaskLoop()
{
    if (ring_payload_flat_.size() == 0 || frames_per_packet == 0)
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
        IPAddress remote_ip;
        uint16_t remote_port = 0;
        bool have_cfg = false;
        if (recfg_ptr_)
        {
            have_cfg = recfg_ptr_ ->isValid();
            if (have_cfg)
            {
                recfg_ptr_->get(remote_ip,remote_port);
            }
        }
        if (recfg_ptr_ && recfg_ptr_->isValid())
        {
            recfg_ptr_->get(remote_ip, remote_port);
            have_cfg = true;
        }
        else
        {
            have_cfg = false;
        }

        bool connected = WiFi_tcp_client_ptr_ ? WiFi_tcp_client_ptr_ -> connected() : false;
        unsigned long now = millis();
        if (!connected)
        {
            if (have_cfg && (WiFi.isConnected()))
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

                    if (tcp_connect_fn_)
                    {
                        ok = tcp_connect_fn_(remote_ip,remote_port);
                    }
                    else if (WiFi_tcp_client_ptr_)
                    {
                        if (have_cfg)
                        {
                            ok = recfg_ptr_->ConnectTOReciverIP(WiFi_tcp_client_ptr_);
                        }

                    }

                    if (ok)
                    {
                        if (consumer_ready_sp_)
                        {
                            consumer_ready_sp_->store(true,std::memory_order_release);
                        }
                        else
                        {
                            if (consumer_ready_sp_)
                            {
                                consumer_ready_sp_->store(false,std::memory_order_release);
                            }
                            vTaskDelay(pdMS_TO_TICKS(20));
                            continue;
                        }
                    }          
                }      
            }
        }

        size_t tail = ring_tail_sp_ ? ring_tail_sp_->load(std::memory_order_acquire) : 0;
        size_t head = ring_head_sp_ ? ring_head_sp_->load(std::memory_order_acquire) : 0;
        if (tail ==head)
        {
            vTaskDelay(pdMS_TO_TICKS(2));
            continue;
        }
        size_t slot = tail & (ring_slots -1);
        uint16_t frames = 0;
        if (ring_frames_span_.size() == ring_slots)
        {
            frames = ring_frames_span_[slot];
        }
        else
        {
            Serial.println("NetworkLoop: ring_frames_span_ not configured");
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }

        if (frames == 0 || frames > (uint16_t)frames_per_packet_)
        {
            if (ring_tail_sp_)
            {
                ring_tail_sp_->store(tail+1, std::memory_order_release);
            }
            continue;
        }
        
        uint32_t seq = sequence_counter_ ? sequence_counter_->fetch_add(1, std::memory_order_relaxed) : 0;
        uint64_t first_index = ring_first_index_span_.size() == ring_slots ? ring_first_index_span_[slot] : 0;
        uint64_t ts = ring_timestamp_span_.size() == ring_slots ? ring_timestamp_span_[slot] : 0;

        if (write_tcp_header_fn_) {
            write_tcp_header_fn_(seq, first_index, ts, (uint16_t)frames);
        } else {
            // implement member write_tcp_header(seq, first_index, ts, frames)
            WriteTCPHeader(seq, first_index, ts, (uint16_t)frames); // default writter
        }
        size_t hsent = 0;
        bool ok = true;
        while (hsent < header_size_)
        {
            int written = 0;
            if (tcp_write_fn_)
            {
                written = tcp_write_fn_(header_buffer_.data() + hsent, header_size_ - hsent);
            }
            else if (WiFi_tcp_client_ptr_)
            {
                written = WiFi_tcp_client_ptr_->write(header_buffer_.data() + hsent, header_size_ - hsent);
            }
            else
            {
                ok = false;
                break;
            }
            if (written <= 0)
            {
                ok = false;
                break;
            }
            if (hsent%1024)
            {
                taskYIELD();
            }
        }
        if (!ok)
        {
            Serial.println("NET: Failed to send header; closing socket and resetting");
            if (tcp_client_stop_fn_)
            {
                tcp_client_stop_fn_();
            }
            if (consumer_ready_sp_)
            {
                consumer_ready_sp_->store(false, std::memory_order_release);
            }
            Ring_clear_Rst();
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }
        
        if (ring_tail_sp_)
        {
            ring_tail_sp_->store(tail+1, std::memory_order_release);
        }
        
        taskYIELD();
    }
}