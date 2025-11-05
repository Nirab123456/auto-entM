#include "headers/audio_read_send.h"
#include <esp_timer.h>
#include "driver/i2s.h"
#include "headers/a_c_s.h"
#include "headers/ReciverConfig.h"   // <<--- add this (exact filename may differ)




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
    if (stopping_check_del("NetworkDataWriterLoop"))
    {
        vTaskDelete(nullptr);
    }

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
            write_ok = recfg_ptr_->TCPWriteAll(WiFi_tcp_client_ptr_, header_ptr, header_len, 4000, 3, 1400);
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

void AUDIO_RS::WriteTCPHeader(
    uint32_t seq,
    uint64_t first_sample_index,
    uint64_t timestamp_us,
    uint16_t number_of_frames
)
{
    header_buffer_[0] = (uint8_t)(HEADER_MAGIC_ & 0xff);
    header_buffer_[1] = (uint8_t)((HEADER_MAGIC_ >> 8) & 0xff);
    header_buffer_[2] = (uint8_t)((HEADER_MAGIC_ >> 16) & 0xff);
    header_buffer_[3] = (uint8_t)((HEADER_MAGIC_ >> 24) & 0xff);
    for (size_t i = 0; i < MIN_BYTES_READ; i++)
    {
        header_buffer_[4+i] = (uint8_t)((seq >> (SIZE_OF_A_BYTE_IN_BITS * i) & 0xff));
    }
    for (size_t i = 0; i < MIN_BYTES_READ * 2; i++)
    {
        header_buffer_[8+i] = (uint8_t)((first_sample_index >> (SIZE_OF_A_BYTE_IN_BITS * i) & 0xff));
    }
    for (size_t i = 0; i < MIN_BYTES_READ * 2; i++)
    {
        header_buffer_[16+i] = (uint8_t)(timestamp_us >> (SIZE_OF_A_BYTE_IN_BITS * i) & 0xff);
    }
    header_buffer_[24] = (uint8_t)(number_of_frames & 0xff);
    header_buffer_[25] = (uint8_t)((number_of_frames >> 8) & 0xff);
    header_buffer_[26] = (uint8_t)CHANNEL_COUNT_->load(std::memory_order_acquire);
    header_buffer_[27] = (uint8_t)(static_cast<std::size_t>(micfg_.i2s_configuration.bits_per_sample / SIZE_OF_A_BYTE_IN_BITS));
    for (size_t i = 0; i < MIN_BYTES_READ; i++)
    {
        header_buffer_[28+i] = (uint8_t)(micfg_.i2s_configuration.sample_rate >> (SIZE_OF_A_BYTE_IN_BITS * i) & 0xff);
    }
    //finish header    
    
}