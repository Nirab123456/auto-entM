#include "headers/audio_read_send.h"
#include <esp_timer.h>
#include "driver/i2s.h"
#include "headers/a_c_s.h"
#include "headers/ReciverConfig.h"
#include <algorithm>

//////////////////////////////////////////////////////////////////////////
// Constructor / setters (unchanged except underscore fixes)
//////////////////////////////////////////////////////////////////////////

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
{ }

void AUDIO_RS::set_consumer_ready(std::shared_ptr<std::atomic<bool>> ar) { consumer_ready_sp_ = std::move(ar); }
void AUDIO_RS::set_ring_head(std::shared_ptr<std::atomic<size_t>> ar) { ring_head_sp_ = std::move(ar); }
void AUDIO_RS::set_ring_tail(std::shared_ptr<std::atomic<size_t>> ar) { ring_tail_sp_ = std::move(ar); }
void AUDIO_RS::set_abs_idx(std::shared_ptr<std::atomic<uint64_t>> ar) { abs_idx_sp_ = std::move(ar); }
void AUDIO_RS::set_i2s_buffer(std::span<uint32_t> i2s_buffer) { i2s_buffer_ = i2s_buffer; }
void AUDIO_RS::set_ring_payload_flat(std::span<uint32_t> flat, size_t frames_per_packet) {
    ring_payload_flat_ = flat;
    frames_per_packet_ = frames_per_packet;
}

//////////////////////////////////////////////////////////////////////////
// Simple trampoline helper for tasks (reuse your existing pattern)
//////////////////////////////////////////////////////////////////////////
void AUDIO_RS::AudioTaskTrampoline(void* pv) {
    AUDIO_RS* self = static_cast<AUDIO_RS*>(pv);
    if (!self) { vTaskDelete(nullptr); return; }
    self->AudioTaskLoop();
}

//////////////////////////////////////////////////////////////////////////
// I2SReaderLoop: reads raw I2S into i2s_buffer_ and enqueues bytes_read
//////////////////////////////////////////////////////////////////////////
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
            // small sleep and retry (yield to other tasks)
            vTaskDelay(pdMS_TO_TICKS(5));
            continue;
        }

        // Non-blocking enqueue: if full, discard oldest element and push new
        if (xQueueSend(i2s_queue_, &bytes_read, 0) != pdTRUE) {
            size_t dummy;
            // Remove oldest and try again
            xQueueReceive(i2s_queue_, &dummy, 0);
            xQueueSend(i2s_queue_, &bytes_read, 0);
        }
    }
}

//////////////////////////////////////////////////////////////////////////
// RingWriterLoop: consumes bytes_read and writes into ring (zero-copy)
// - writes per-slot metadata BEFORE publishing head
// - enqueues slot into network_slot_queue_ (non-blocking)
//////////////////////////////////////////////////////////////////////////
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

    for (;;) {
        size_t bytes_read = 0;
        if (xQueueReceive(i2s_queue_, &bytes_read, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        size_t word_count = bytes_read / sizeof(uint32_t);
        size_t available_frames = 0;
        if (word_count >= (frames * DEFAULT_CHANNEL_COUNT)) {
            available_frames = frames;
        } else {
            available_frames = (word_count / DEFAULT_CHANNEL_COUNT);
        }
        if (available_frames > frames) available_frames = frames;

        size_t head = ring_head_sp_ ? ring_head_sp_->load(std::memory_order_relaxed) : 0;
        size_t tail = ring_tail_sp_ ? ring_tail_sp_->load(std::memory_order_acquire) : 0;
        size_t next_head = head + 1;

        // Check ring space
        if ((next_head - tail) > ring_slots) {
            // Overrun handling
            if (overrun_policy_ == OverRunPolicy::DROP_NEWEST) {
                drop_count_newest_.fetch_add(1, std::memory_order_relaxed);
                if (abs_idx_sp_) {
                    abs_idx_sp_->fetch_add((uint64_t)available_frames, std::memory_order_relaxed);
                }
                taskYIELD();
                continue;
            } else { // DROP_OLDEST
                if (ring_tail_sp_) {
                    ring_tail_sp_->fetch_add(1, std::memory_order_acq_rel);
                    drop_count_oldest_.fetch_add(1, std::memory_order_relaxed);
                    tail = ring_tail_sp_->load(std::memory_order_acquire);
                } else {
                    drop_count_newest_.fetch_add(1, std::memory_order_relaxed);
                    if (abs_idx_sp_) abs_idx_sp_->fetch_add((uint64_t)available_frames, std::memory_order_relaxed);
                    taskYIELD();
                    continue;
                }
            }
        }

        // compute slot & row pointer
        size_t slot = ring_power_of_two ? (head & ring_mask) : (head % ring_slots);
        uint32_t* row_ptr = ring_payload_flat_.data() + slot * frames;
        std::span<uint32_t> row(row_ptr, frames);

        // Copy samples into row: handle available_frames < frames
        for (size_t i = 0; i < available_frames; ++i) {
            // pick channel 0 (consistent selection). If you prefer channel 1, change index.
            row[i] = i2s_buffer_[i * DEFAULT_CHANNEL_COUNT + 0];
        }
        // zero-fill remainder
        for (size_t i = available_frames; i < frames; ++i) row[i] = 0u;

        // metadata: capture first sample index and timestamp (use relaxed load for abs_idx)
        uint64_t first_sample_idx = abs_idx_sp_ ? abs_idx_sp_->load(std::memory_order_relaxed) : 0;
        uint64_t ts = (uint64_t)esp_timer_get_time();

        // Write metadata into spans BEFORE publishing head
        if (ring_frames_span_.size() == ring_slots) {
            ring_frames_span_[slot] = static_cast<uint16_t>(available_frames);
        }
        if (ring_first_index_span_.size() == ring_slots) {
            ring_first_index_span_[slot] = first_sample_idx;
        }
        if (ring_timestamp_span_.size() == ring_slots) {
            ring_timestamp_span_[slot] = ts;
        }

        // publish head and advance absolute sample index
        if (ring_head_sp_) ring_head_sp_->store(next_head, std::memory_order_release);
        if (abs_idx_sp_) abs_idx_sp_->fetch_add((uint64_t)available_frames, std::memory_order_relaxed);

        // notify network writer (non-blocking)
        if (network_slot_queue_) {
            size_t s = slot;
            if (xQueueSend(network_slot_queue_, &s, 0) != pdTRUE) {
                // queue full -> drop notification (monitor drop_count if desired)
                // drop_notify_count_.fetch_add(1, std::memory_order_relaxed);
            }
        }

        taskYIELD();
    } // for
}

//////////////////////////////////////////////////////////////////////////
// Ring_clear_Rst: clear ring and reset counters (name matches header)
//////////////////////////////////////////////////////////////////////////
void AUDIO_RS::Ring_clear_Rst()
{
    // quick sanity: if nothing configured, just return
    if (ring_payload_flat_.size() == 0 &&
        i2s_buffer_.size() == 0 &&
        !ring_head_sp_ &&
        !ring_tail_sp_ &&
        !abs_idx_sp_) {
        return;
    }

    // suspend scheduler briefly to avoid partial writes (small critical window)
    vTaskSuspendAll();

    if (ring_payload_flat_.size() > 0) {
        std::fill(ring_payload_flat_.begin(), ring_payload_flat_.end(), 0u);
    }
    if (i2s_buffer_.size() > 0) {
        std::fill(i2s_buffer_.begin(), i2s_buffer_.end(), 0u);
    }

    if (ring_tail_sp_) ring_tail_sp_->store(0, std::memory_order_relaxed);
    if (ring_head_sp_) ring_head_sp_->store(0, std::memory_order_relaxed);
    if (abs_idx_sp_) abs_idx_sp_->store(0, std::memory_order_relaxed);

    drop_count_newest_.store(0u, std::memory_order_relaxed);
    drop_count_oldest_.store(0u, std::memory_order_relaxed);

    // restore scheduler
    xTaskResumeAll();
}

//////////////////////////////////////////////////////////////////////////
// NetworkTaskLoop becomes the controller: ensures connection & enqueues tail slots
//////////////////////////////////////////////////////////////////////////
void AUDIO_RS::NetworkTaskLoop()
{
    if (ring_payload_flat_.size() == 0 || frames_per_packet_ == 0) {
        Serial.println("NetworkLoop: ring or frames not configured");
        vTaskDelay(pdMS_TO_TICKS(100));
        vTaskDelete(nullptr);
        return;
    }

    const size_t ring_slots = ring_payload_flat_.size() / frames_per_packet_;
    if (ring_slots == 0) {
        Serial.println("NetworkTaskLoop : invalid ring_slots");
        vTaskDelay(pdMS_TO_TICKS(100));
        vTaskDelete(nullptr);
        return;
    }

    unsigned long last_conn_attempt = 0;

    for (;;) {
        // Fetch configuration if available
        IPAddress remote_ip; uint16_t remote_port = 0;
        bool have_cfg = (recfg_ptr_ && recfg_ptr_->isValid());
        if (have_cfg) recfg_ptr_->get(remote_ip, remote_port);

        bool connected = WiFi_tcp_client_ptr_ ? WiFi_tcp_client_ptr_->connected() : false;
        unsigned long now = millis();

        // Attempt connection periodically if not connected
        if (!connected) {
            if (have_cfg && WiFi.isConnected()) {
                if ((now - last_conn_attempt) >= CONNECTION_RETRY_INTERVAL_MS) {
                    last_conn_attempt = now;
                    if (consumer_ready_sp_) consumer_ready_sp_->store(false, std::memory_order_release);

                    // reset ring (so we won't send stale slots)
                    Ring_clear_Rst();

                    bool ok = false;
                    if (tcp_connect_fn_) {
                        ok = tcp_connect_fn_(remote_ip, remote_port);
                    } else if (WiFi_tcp_client_ptr_) {
                        ok = recfg_ptr_ ? recfg_ptr_->ConnectTOReciverIP(WiFi_tcp_client_ptr_) : false;
                    }

                    if (ok) {
                        if (consumer_ready_sp_) consumer_ready_sp_->store(true, std::memory_order_release);
                    }
                }
            }
        } // connection handling

        // If not connected, wait a bit
        if (!WiFi_tcp_client_ptr_ || !WiFi_tcp_client_ptr_->connected()) {
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }

        // If no work (tail == head) sleep briefly
        size_t tail = ring_tail_sp_ ? ring_tail_sp_->load(std::memory_order_acquire) : 0;
        size_t head = ring_head_sp_ ? ring_head_sp_->load(std::memory_order_acquire) : 0;
        if (tail == head) {
            vTaskDelay(pdMS_TO_TICKS(2));
            continue;
        }

        // Enqueue the next tail slot for network writer
        size_t slot = tail & (ring_slots - 1);
        // sanity check frames metadata
        uint16_t frames = (ring_frames_span_.size() == ring_slots) ? ring_frames_span_[slot] : 0;
        if (frames == 0 || frames > (uint16_t)frames_per_packet_) {
            // advance tail to avoid livelock if invalid
            if (ring_tail_sp_) ring_tail_sp_->store(tail + 1, std::memory_order_release);
            continue;
        }

        // Try to enqueue slot for the network writer (short timeout)
        if (network_slot_queue_) {
            if (xQueueSend(network_slot_queue_, &slot, pdMS_TO_TICKS(5)) != pdTRUE) {
                // queue full: let network writer catch up, do not drop tail here
                vTaskDelay(pdMS_TO_TICKS(2));
            } else {
                // enqueued successfully; network writer will advance tail after send
            }
        } else {
            // No network queue: as fallback, advance tail to avoid stalling
            if (ring_tail_sp_) ring_tail_sp_->store(tail + 1, std::memory_order_release);
        }

        taskYIELD();
    }
}

//////////////////////////////////////////////////////////////////////////
// NetworkDataWriterLoop: robustly writes header+payload for each enqueued slot
// Uses recfg_ptr_->tcpWriteAll(...) to handle partial writes/reconnects.
//////////////////////////////////////////////////////////////////////////
void AUDIO_RS::NetworkDataWriterLoop()
{
    if (!network_slot_queue_) {
        vTaskDelay(pdMS_TO_TICKS(100));
        vTaskDelete(nullptr);
        return;
    }

    const size_t ring_slots = (ring_payload_flat_.size() / frames_per_packet_);
    if (ring_slots == 0) {
        vTaskDelay(pdMS_TO_TICKS(100));
        vTaskDelete(nullptr);
        return;
    }

    for (;;) {
        size_t slot = 0;
        if (xQueueReceive(network_slot_queue_, &slot, portMAX_DELAY) != pdTRUE) continue;

        if (slot >= ring_slots) continue; // sanity

        // ensure connection is available
        if (!recfg_ptr_ || !recfg_ptr_->isValid() || !WiFi.isConnected() || !WiFi_tcp_client_ptr_) {
            // backoff and retry later
            vTaskDelay(pdMS_TO_TICKS(50));
            // optionally re-enqueue: xQueueSendToFront(network_slot_queue_, &slot, 0);
            continue;
        }

        // get per-slot metadata
        const uint16_t frames = (ring_frames_span_.size() == ring_slots) ? ring_frames_span_[slot] : 0;
        const uint64_t first_index = (ring_first_index_span_.size() == ring_slots) ? ring_first_index_span_[slot] : 0;
        const uint64_t ts = (ring_timestamp_span_.size() == ring_slots) ? ring_timestamp_span_[slot] : 0;

        if (frames == 0 || frames > (uint16_t)frames_per_packet_) {
            // invalid -> advance tail and skip
            size_t tail = ring_tail_sp_ ? ring_tail_sp_->load(std::memory_order_acquire) : 0;
            if (ring_tail_sp_) ring_tail_sp_->store(tail + 1, std::memory_order_release);
            continue;
        }

        // Build header into header_buffer_ (callback or default)
        uint32_t seq = sequence_counter_ ? sequence_counter_->fetch_add(1, std::memory_order_relaxed) : 0;
        if (write_tcp_header_fn_) write_tcp_header_fn_(seq, first_index, ts, frames);
        else WriteTCPHeader(seq, first_index, ts, frames); // assume this writes into header_buffer_

        uint8_t* header_ptr = header_buffer_.data();
        size_t header_len = header_size_;

        // payload pointer & size
        uint32_t* row_ptr = ring_payload_flat_.data() + slot * frames_per_packet_;
        uint8_t* payload_ptr = reinterpret_cast<uint8_t*>(row_ptr);
        size_t payload_len = (size_t)frames * sizeof(uint32_t);

        bool success = false;
        // Attempt robust writes via ReciverConfig helper (which should implement tcpWriteAll)
        if (recfg_ptr_) {
            // ensure client connected
            if (!WiFi_tcp_client_ptr_ || !WiFi_tcp_client_ptr_->connected()) {
                if (!recfg_ptr_->ConnectTOReciverIP(WiFi_tcp_client_ptr_)) {
                    // connection failed; requeue / backoff
                    vTaskDelay(pdMS_TO_TICKS(50));
                    continue;
                }
            }

            // header
            if (!recfg_ptr_->TCPWriteAll(WiFi_tcp_client_ptr_, header_ptr, header_len, 2000, 3, 1400)) {
                success = false;
            } else {
                // payload
                if (!recfg_ptr_->TCPWriteAll(WiFi_tcp_client_ptr_, payload_ptr, payload_len, 4000, 3, 1400)) {
                    success = false;
                } else {
                    success = true;
                }
            }
        } else {
            // no recfg helper — best-effort with WiFiClient
            if (WiFi_tcp_client_ptr_ && WiFi_tcp_client_ptr_->connected()) {
                size_t sent_h = WiFi_tcp_client_ptr_->write(header_ptr, header_len);
                if (sent_h == header_len) {
                    size_t sent_p = WiFi_tcp_client_ptr_->write(payload_ptr, payload_len);
                    success = (sent_p == payload_len);
                } else success = false;
            } else success = false;
        }

        if (!success) {
            // failure: close socket and reset flags
            if (tcp_client_stop_fn_) tcp_client_stop_fn_();
            if (WiFi_tcp_client_ptr_) WiFi_tcp_client_ptr_->stop();
            if (consumer_ready_sp_) consumer_ready_sp_->store(false, std::memory_order_release);
            Ring_clear_Rst();
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        // on success: advance tail
        if (ring_tail_sp_) {
            size_t tail = ring_tail_sp_->load(std::memory_order_acquire);
            ring_tail_sp_->store(tail + 1, std::memory_order_release);
        }

        taskYIELD();
    } // for ever
}
