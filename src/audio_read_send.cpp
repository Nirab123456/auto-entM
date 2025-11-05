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
                          void* arg,
                          TaskHandle_t* out_handle=nullptr
                        )
{
    void* pv_arg = (arg != nullptr) ? arg : this;
    TaskHandle_t created_handle = nullptr;
    BaseType_t ok;
    if (core > 0)
    {
        ok = xTaskCreatePinnedToCore(
            trampoline,
            name,
            stack,
            pv_arg,
            prio,
            &created_handle,
            core
        );
    }
    else
    {
        ok = xTaskCreate(
            trampoline,
            name,
            stack,
            pv_arg,
            prio,
            &created_handle
        );
    }
    if (ok ==pdPASS && created_handle != nullptr)
    {
        if (out_handle)
        {
            *out_handle = created_handle;
        }
        // // optional heuristic: fill first empty member slot (safer: caller should set)
        // if (!i2s_reader_handle_) i2s_reader_handle_ = created_handle;
        // else if (!ring_writer_handle_) ring_writer_handle_ = created_handle;
        // else if (!network_handle_) network_handle_ = created_handle;
        // else if (!network_writer_handle_) network_writer_handle_ = created_handle;
        // else if (!monitor_handle_) monitor_handle_ = created_handle;
        return true;
    }
    return false;
}

bool AUDIO_RS::IsKnownHandle(TaskHandle_t h) const
{
    return (
            h == i2s_reader_handle_ ||
            h == ring_writer_handle_ ||
            h == network_handle_ ||
            h == network_writer_handle_ ||
            h == monitor_handle_
    );
}

void AUDIO_RS::stop_task(TaskHandle_t handle, TickType_t wait_ms)
{
    stopping_.store(true, std::memory_order_release);

    if (consumer_ready_sp_)
    {
        consumer_ready_sp_->store(false,std::memory_order_release);
    }

    if (WiFi_tcp_client_ptr_ && WiFi_tcp_client_ptr_->connected())
    {
        WiFi_tcp_client_ptr_->stop();
    }
    
    if (i2s_queue_)
    {
        xQueueReset(i2s_queue_);
    }
    if (network_slot_queue_)
    {
        xQueueReset(network_slot_queue_);
    }
    if (network_slot_queue_) {
        // optional: send sentinel value, depends on your design
    }    
    std::vector<TaskHandle_t> targets;
    if (handle == nullptr)
    {
        if (i2s_reader_handle_)
        {
            targets.push_back(i2s_reader_handle_);
        }
        if (ring_writer_handle_)
        {
            targets.push_back(ring_writer_handle_);
        }
        if (network_handle_)
        {
            targets.push_back(network_handle_);
        }
        if (network_writer_handle_)
        {
            targets.push_back(network_writer_handle_);
        }
        if (monitor_handle_)
        {
            targets.push_back(monitor_handle_);
        }
    }
    else
    {
        if (!IsKnownHandle(handle))
        {
            Serial.printf("Warning: stop_task() unknown handle %p — ignoring\n", (void*)handle);
            return;           
        }
        targets.push_back(handle);
    }
    std::sort(targets.begin(), targets.end());
    targets.erase(std::unique(targets.begin(), targets.end()), targets.end());

    for (TaskHandle_t h : targets)
    {
        if (h == nullptr)
        {
            continue;
        }
        if (h == xTaskGetCurrentTaskHandle())
        {
            vTaskDelay(pdMS_TO_TICKS(10));
            vTaskDelete(nullptr);
            continue;
        }
        
        xTaskNotifyGive(h);

        const TickType_t start = xTaskGetTickCount();
        const TickType_t wait_ticks = (wait_ms == portMAX_DELAY) ? portMAX_DELAY : pdMS_TO_TICKS(wait_ms);
        bool deleted = false;
        while ((xTaskGetTickCount() - start) < wait_ticks)
        {
            eTaskState st = eTaskGetState(h);
            if (st == eDeleted)
            {
                deleted = true;
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(10));
        }
        if (!deleted)
        {
            vTaskDelete(h);
        }
        ClearHandleField(h);
    }
    
    stopping_.store(false, std::memory_order_release);
    

}

void AUDIO_RS::I2SReaderLoop()
{
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
        esp_err_t err = i2s_read(
            static_cast<i2s_port_t>(i2s_port_),
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
        BaseType_t sent = xQueueSend(i2s_queue_, &bytes_to_read, 0);
        if (sent != pdTRUE)
        {
            if (overrun_policy_ == OverRunPolicy::DROP_OLDEST)
            {
                size_t dropped;
                if (xQueueReceive(i2s_queue_, &dropped, 0) == pdTRUE)
                {
                    // we dropped 'dropped' (bytes count) older sample
                }
                if (xQueueSend(i2s_queue_, &bytes_to_read, 0) !=pdTRUE)
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

// Initialize I2S for capture. Returns true on success.
bool AUDIO_RS::initI2S()
{
    // store port for later deinit
    if (!mic_configured_.load(std::memory_order_acquire))
    {
        return false;
    }
    

    // Sanity: we need frames_per_packet_ to compute DMA sizing
    if (frames_per_packet_ == 0) {
        Serial.println("I2S init failed: frames_per_packet_ == 0");
        return false;
    }

    // If a driver is already installed on this port, uninstall first (clean slate)
    if (i2s_installed_) {
        i2s_driver_uninstall(micfg_.i2s_port);
        i2s_installed_ = false;
    }

    // Compute a reasonable dma_buf_len.
    // dma_buf_len is the number of "samples" per DMA buffer — choose a fraction of frames_per_packet_.
    // Keep it at least small (4) and not excessively large.
    size_t dma_buf_len = frames_per_packet_ / 2;
    if (dma_buf_len < 4) dma_buf_len = 4;
    // optionally clamp to a maximum (e.g., 2048) to avoid huge allocations
    if (dma_buf_len > 2048) dma_buf_len = 2048;

    // number of DMA buffers (tune for latency vs memory)
    int dma_buf_count = 6;

    // Configure I2S driver
    i2s_config_t i2s_config = micfg_.i2s_configuration;
    // pin config - using your board-level constants
    i2s_pin_config_t pin_config = micfg_.i2spinconfiguration;

    esp_err_t err;

    // Install driver
    err = i2s_driver_install(micfg_.i2s_port, &i2s_config, 0, nullptr);
    if (err != ESP_OK) {
        Serial.printf("I2S: driver install failed (err %d)\n", (int)err);
        return false;
    }

    // Set pins
    err = i2s_set_pin(micfg_.i2s_port, &pin_config);
    if (err != ESP_OK) {
        Serial.printf("I2S: set_pin failed (err %d) - uninstalling driver\n", (int)err);
        i2s_driver_uninstall(micfg_.i2s_port);
        return false;
    }

    // Clear DMA buffers
    i2s_zero_dma_buffer(micfg_.i2s_port);

    i2s_installed_ = true;
    Serial.printf("I2S: initialized on port %d (dma_buf_count=%d dma_buf_len=%u)\n", micfg_.i2s_port, dma_buf_count, (unsigned)dma_buf_len);
    return true;
}

void AUDIO_RS::deinitI2S()
{
    if (!i2s_installed_) return;
    // Stop any I2S activity as needed - driver uninstall handles it
    i2s_driver_uninstall(micfg_.i2s_port);
    i2s_installed_ = false;
    Serial.println("I2S: driver uninstalled");
}

bool MicrophoneConfig::validate(char* err)
{
    if (!(i2s_port == I2S_NUM_0 || i2s_port == I2S_NUM_1))
    {
        err = "MicrophoneConfig::Wrong I2s port";
        return false;
    }

    if ((i2s_configuration.mode & I2S_MODE_RX) == 0)
    {
        err = "MicrophoneConfig::i2s config must include I2s_MODE_RX";
        return false;
    }
    
    if (!(
        i2s_configuration.bits_per_sample == I2S_BITS_PER_CHAN_16BIT ||
        i2s_configuration.bits_per_sample == I2S_BITS_PER_CHAN_24BIT ||
        i2s_configuration.bits_per_sample == I2S_BITS_PER_CHAN_32BIT
    ))
    {
        err = "MicrophoneConfig::unsupported bits_per_sample";
        return false;
    }
    if (i2s_configuration.dma_buf_count < 1 || i2s_configuration.dma_buf_count > 12)
    {
        err = "MicrophoneConfig:: dma_buf_count out of range";
        return false;
    }
    if (i2s_configuration.dma_buf_len < 4 || i2s_configuration.dma_buf_len > 8192) {
        err = "dma_buf_len out of range (4..8192).";
        return false;
    }

    auto invalid_pin = [](int p)->bool
    {
        return (p == I2S_PIN_NO_CHANGE || p < 0 || p > 39);
    };
    if (
        invalid_pin(i2spinconfiguration.bck_io_num) || 
        invalid_pin(i2spinconfiguration.ws_io_num) ||
        i2s_configuration.mode & I2S_MODE_RX ? invalid_pin(i2spinconfiguration.data_in_num) : false
    )
    {
        err = "MicrophoneConfig::i2s pins must be within bound (0 - 39) or board spesific";
        return false;   
    }
    
}