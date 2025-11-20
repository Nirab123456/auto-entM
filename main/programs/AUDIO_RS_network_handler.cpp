#include "headers/audio_read_send.h"
#include <esp_timer.h>
#include "driver/i2s_std.h"
#include "headers/a_c_s.h"
#include "headers/ReciverConfig.h"   // <<--- add this (exact filename may differ)

static const char *nhTAG = "AUDIO_RS_network_handler";

bool AUDIO_RS::set_network_slot_queue(QueueHandle_t q)
{
    if (q != nullptr)
    {
        network_slot_queue_ = q;
        return true;
    }
    return false;
}

void AUDIO_RS::set_WiFi_client_ptr(WiFiClient* client)
{
    if (client)
    {
        WiFi_tcp_client_ptr_ = client;
    }
    
}

void AUDIO_RS::set_reciver_config_ptr(ReciverConfig* ptr)
{
    if (ptr)
    {
        recfg_ptr_ = ptr;
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
    if (stopping_check_del("NetworkTaskLoop"))
    {
        vTaskDelete(nullptr);
    }

    if (ring_payload_flat_.size() == 0 || frames_per_packet_ == 0)
    {
        ESP_LOGE(nhTAG, "NetworkLoop: ring or frames not configured");
        vTaskDelay(pdMS_TO_TICKS(100));
        vTaskDelete(nullptr);
        return;
    }

    const size_t ring_slots = ring_payload_flat_.size() /frames_per_packet_;
    if (ring_slots == 0)
    {
        ESP_LOGE(nhTAG, "NetworkTaskLoop : invalid ring_slots");
        vTaskDelay(pdMS_TO_TICKS(100));
        vTaskDelete(nullptr);
        return;
    }
    
    unsigned long last_conn_attempt = 0;

    for (;;)
    {
        if (stopping_.load(std::memory_order_acquire))
        {
            break;
        }

        size_t tail = ring_tail_sp_ ? ring_tail_sp_->load(std::memory_order_acquire) : 0;
        size_t head = ring_head_sp_ ? ring_head_sp_->load(std::memory_order_acquire) : 0;

        bool have_cfg = (recfg_ptr_ && recfg_ptr_->isValid());
        bool connected = (WiFi_tcp_client_ptr_ && WiFi_tcp_client_ptr_->connected());
        unsigned long now = millis();

        if (!connected)
        {
            if (head == tail)
            {
                vTaskDelay(pdMS_TO_TICKS(50));
                continue;
            }
            
            if (have_cfg && WiFi.isConnected())
            {
                uint32_t fails = connection_failure_.load(std::memory_order_relaxed);
                uint64_t attempt_delay = (uint64_t) conn_retry_base_ms_ << std:: min<uint32_t> (fails, 6u);
                if (attempt_delay > conn_retry_max_ms_)
                {
                    attempt_delay = conn_retry_max_ms_;
                }
                
                if ((now - last_conn_attempt) >= static_cast<unsigned long>(attempt_delay))
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
                        connection_failure_.store(0,std::memory_order_relaxed);

                        if (network_writer_handle_)
                        {
                            xTaskNotifyGive(network_writer_handle_);
                        }
                        ESP_LOGE(nhTAG, "NetworkTaskLoop: connected to receiver (waking writer)");
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

        tail = ring_tail_sp_ ? ring_tail_sp_->load(std::memory_order_acquire) : 0;
        head = ring_head_sp_ ? ring_head_sp_->load(std::memory_order_acquire) : 0;
        if (tail == head)
        {
            vTaskDelay(pdMS_TO_TICKS(2));
            continue;
        }
        

        size_t slot = tail & (ring_slots -1);

        if (ring_frames_span_.size() != ring_slots)
        {
            ESP_LOGD(nhTAG, "NetworkTaskLoop: ring_frames_span_ not configured");
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
                vTaskDelay(pdMS_TO_TICKS(1));
            }
            else
            {
                //sussess
            }
            
            
        }
        vTaskDelay(pdMS_TO_TICKS(1));
    }
    vTaskDelete(nullptr);
}

void AUDIO_RS::NetworkDataWriterLoop()
{
    ESP_LOGI(nhTAG, "AUDIO_RS::NetworkDataWriterLoop");
    if (stopping_check_del("NetworkDataWriterLoop"))
    {
        vTaskDelete(nullptr);
    }

    if (network_slot_queue_ == nullptr) {
        ESP_LOGE(nhTAG, "AUDIO_RS::NetworkDataWriterLoop ->network_slot_queue_:: nullptr");
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

        std::vector<uint8_t> header_copy;
        size_t header_len_local = 0;
        header_len_local = header_size_.load(std::memory_order_acquire);
        {
            std::lock_guard<std::mutex> lk(header_mu_);
            if (header_len_local > 0)
            {
                header_copy.assign(header_buffer_.begin(), header_buffer_.begin() + header_len_local);
            }
        }

        uint8_t* header_ptr = header_copy.empty() ? nullptr : header_copy.data();
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
            write_ok = recfg_ptr_->TCPWriteAll(WiFi_tcp_client_ptr_, header_ptr, header_len_local, 4000, 3, 1400);
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
            connection_failure_.fetch_add(1,std::memory_order_relaxed);            
            xQueueSendToBack(network_slot_queue_, &slot, 0);
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        connection_failure_.store(0,std::memory_order_relaxed);
        if (consumer_ready_sp_ && consumer_ready_sp_->load(std::memory_order_acquire))
        {
            consumer_ready_sp_->store(true, std::memory_order_release);
            ESP_LOGI(nhTAG, "NetworkDataWriterLoop: consumer_ready set true after successful write");
        }

        
        if (ring_tail_sp_)
        {
            size_t tail = ring_tail_sp_->load(std::memory_order_acquire);
            ring_tail_sp_->store(tail+1, std::memory_order_release);
        }
        taskYIELD();
    }
    vTaskDelete(nullptr);
}

void AUDIO_RS::WriteTCPHeader(
    uint32_t seq,
    uint64_t first_sample_index,
    uint64_t timestamp_us,
    uint16_t number_of_frames
)
{
    constexpr size_t THIS_HEADER_SIZE = 34;
    size_t required = std::max<size_t>(THIS_HEADER_SIZE, 0);
    {
        std::lock_guard<std::mutex> lk(header_mu_);
        if (header_buffer_.size() < required)
        {
            header_buffer_.assign(required,0);
        }

        uint8_t* hdrptr = header_buffer_.data();
        size_t off = 0;
        uint32_t magic = HEADER_MAGIC_;
        hdrptr[off + 0] = (uint8_t) (magic & 0xff);
        hdrptr[off + 1] = (uint8_t) ((magic >> 8) & 0xff);
        hdrptr[off + 2] = (uint8_t) ((magic >> 16) & 0xff);
        hdrptr[off + 3] = (uint8_t) ((magic >> 24) & 0xff); 
        off += MIN_BYTES_READ;
        for (size_t i = 0; i < MIN_BYTES_READ; i++)
        {
            hdrptr[off + i] = static_cast<uint8_t>((seq >> SIZE_OF_A_BYTE_IN_BITS*i) & 0xff);
        }
        off += MIN_BYTES_READ;
        for (size_t i = 0; i < MIN_BYTES_READ * 2; i++)
        {
            hdrptr[off + i] = static_cast<uint8_t>((first_sample_index >> SIZE_OF_A_BYTE_IN_BITS * i) & 0xff);
        }
        off += (MIN_BYTES_READ * 2);
        for (size_t i = 0; i < MIN_BYTES_READ * 2; i++)
        {
            hdrptr[off + i] = static_cast<uint8_t>((timestamp_us >> SIZE_OF_A_BYTE_IN_BITS * i) & 0xff);
        }
        off += (MIN_BYTES_READ * 2);
        hdrptr[off + 0] = static_cast<uint8_t>(number_of_frames & 0xff);
        hdrptr[off + 1] = static_cast<uint8_t>((number_of_frames >> 8) & 0xff);
        off += 2; //off += sizeof(number_of_frames) / SIZE_OF_A_BYTE_IN_BITS;
        uint8_t cc = 1;
        if (CHANNEL_COUNT_)
        {
            cc = static_cast<uint8_t>(CHANNEL_COUNT_->load(std::memory_order_acquire));
        }
        hdrptr[off++] = cc;

        uint8_t slot_width_bytes = (micfg_.SlotBitWidth_ > 0) ? static_cast<uint8_t>(micfg_.SlotBitWidth_ / SIZE_OF_A_BYTE_IN_BITS) : 3; // have to change magic number 3
        hdrptr[off++] = slot_width_bytes;

        uint32_t sample_rate = 0;
        if (micfg_.i2s_configuration.clk_cfg.sample_rate_hz > 0)
        {
            sample_rate = micfg_.i2s_configuration.clk_cfg.sample_rate_hz;
        }
        else
        {
            sample_rate = 48000;
        }
        for (size_t i = 0; i < MIN_BYTES_READ; i++)
        {
            hdrptr[off + i] = static_cast<uint8_t>((sample_rate >> SIZE_OF_A_BYTE_IN_BITS * i) & 0xff);
        }
        off += MIN_BYTES_READ;
        hdrptr[off + 0] = static_cast<uint8_t>(FORMAT_INT32_LEFT24_ & 0xff);
        hdrptr[off + 1] = static_cast<uint8_t>((FORMAT_INT32_LEFT24_ >> SIZE_OF_A_BYTE_IN_BITS) & 0xff);
        off +=2;
        header_size_.store(off, std::memory_order_release);

        ESP_LOGD(nhTAG,"AUDIO_RS::WriteTCPHeader:Wrritten = %i bytes",(int)off);
    }

    
}

void AUDIO_RS::set_header_buffer_size(size_t n)
{
    std::lock_guard<std::mutex> lk(header_mu_);
    header_size_.store(n, std::memory_order_release);
    if (n == 0)
    {
        header_buffer_.clear();
        header_buffer_.shrink_to_fit();
    }else
    {
        header_buffer_.assign(n, 0);
    }
}