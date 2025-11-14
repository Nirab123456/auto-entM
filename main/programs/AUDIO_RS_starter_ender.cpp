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
    if (core >= 0)
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

bool AUDIO_RS::stopping_check_del(char* taskname)
{
    if (stopping_.load(std::memory_order_acquire))
    {
        Serial.printf("AUDIO_RS::%s stopping\n", taskname);
        return true;
    }
    return false;
}

void AUDIO_RS::set_ring_metadata_spans(
    std::span<uint16_t> frames_span,
    std::span<uint64_t> first_index_span,
    std::span<uint64_t> ts_us_span
)
{
    ring_frames_span_ = frames_span;
    ring_first_index_span_ = first_index_span;
    ring_timestamp_span_ = ts_us_span;

    if (ring_payload_flat_.size() != 0 && frames_per_packet_ != 0)
    {
        size_t ring_slots = ring_payload_flat_.size() / frames_per_packet_;
        if (ring_slots == 0)
        {
            Serial.println("AUDIO_RS::set_ring_metadata_spans: warning - computed ring_slots == 0");
        }
        else
        {
            bool ok = true;
            if (ring_frames_span_.size() != ring_slots) {
                Serial.printf("AUDIO_RS::set_ring_metadata_spans: warning ring_frames_span size %u != ring_slots %u\n",
                              (unsigned)ring_frames_span_.size(), (unsigned)ring_slots);
                ok = false;
            }
            if (ring_first_index_span_.size() != ring_slots) {
                Serial.printf("AUDIO_RS::set_ring_metadata_spans: warning ring_first_index_span size %u != ring_slots %u\n",
                              (unsigned)ring_first_index_span_.size(), (unsigned)ring_slots);
                ok = false;
            }
            if (ring_timestamp_span_.size() != ring_slots) {
                Serial.printf("AUDIO_RS::set_ring_metadata_spans: warning ring_timestamp_span size %u != ring_slots %u\n",
                              (unsigned)ring_timestamp_span_.size(), (unsigned)ring_slots);
                ok = false;
            }
            if (!ok)
            {
                Serial.println("AUDIO_RS::set_ring_metadata_spans: metadata spans mismatch — please provide arrays with length == ring_slots");
            }
            else
            {
                Serial.println("AUDIO_RS::set_ring_metadata_spans: metadata spans configured OK");
            }
        }
    }
}


void AUDIO_RS::set_sequence_counter(std::shared_ptr<std::atomic<uint32_t>> seq)
{
    sequence_counter_ = seq;
}


void AUDIO_RS::PauseNetworkStreaming()
{
    if (consumer_ready_sp_)
    {
        consumer_ready_sp_->store(false, std::memory_order_release);
    }
    if (WiFi_tcp_client_ptr_)
    {
        if (WiFi_tcp_client_ptr_->connected())
        {
            WiFi_tcp_client_ptr_->stop();
        }
    }
    Serial.println("AUDIO_RS::PauseNetworkStreaming: consumer_ready cleared and tcp client stopped");    
}

bool AUDIO_RS::ReqNetworkReconnect()
{
    if (!WiFi_tcp_client_ptr_) {
        Serial.println("AUDIO_RS::ReqNetworkReconnect: no WiFiClient configured");
        return false;
    }
    if (!recfg_ptr_) {
        Serial.println("AUDIO_RS::ReqNetworkReconnect: no ReciverConfig pointer set");
        return false;
    }

    bool ok = recfg_ptr_->ConnectTOReciverIP(WiFi_tcp_client_ptr_);
    if (ok)
    {
        Serial.println("AUDIO_RS::ReqNetworkReconnect:Reconnect successfull");
        if (consumer_ready_sp_)
        {
            consumer_ready_sp_->store(true,std::memory_order_release);
        }
        return true;
    }
    else
    {
        Serial.println("AUDIO_RS::request_network_reconnect: reconnect failed");
        return false;
    }
}

void AUDIO_RS::set_consumer_ready_flag(bool v)
{
    if (consumer_ready_sp_)
    {
        consumer_ready_sp_->store(v,std::memory_order_release);
    }
    else
    {
        consumer_ready_sp_ = std::make_shared<std::atomic<bool>>(v);
    }
}