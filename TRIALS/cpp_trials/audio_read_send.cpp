#include "audio_read_send.h"
#include <esp_timer.h>
#include "driver/i2s.h"
#include "audio_conf_sett.h"

// NOTE: requires your audio_conf_sett.h for constants
// #include "audio_conf_sett.h"

// ---- ctor ----
AUDIO_RS::AUDIO_RS(std::span<uint32_t> i2s_buffer,
                   std::span<uint32_t> ring_payload_flat,
                   size_t frames_per_packet,
                   std::shared_ptr<std::atomic<bool>> consumer_ready,
                   std::shared_ptr<std::atomic<size_t>> ring_head,
                   std::shared_ptr<std::atomic<size_t>> ring_tail,
                   std::shared_ptr<std::atomic<uint64_t>> abs_idx)
  : i2s_buffer_(i2s_buffer),
    ring_payload_flat_(ring_payload_flat),
    frames_per_packet_(frames_per_packet),
    consumer_ready_sp_(std::move(consumer_ready)),
    ring_head_sp_(std::move(ring_head)),
    ring_tail_sp_(std::move(ring_tail)),
    abs_idx_sp_(std::move(abs_idx)),
    i2s_queue_(nullptr)
{}

// ---- setters ----
void AUDIO_RS::set_consumer_ready(std::shared_ptr<std::atomic<bool>> ar) { consumer_ready_sp_ = std::move(ar); }
void AUDIO_RS::set_ring_head(std::shared_ptr<std::atomic<size_t>> ar) { ring_head_sp_ = std::move(ar); }
void AUDIO_RS::set_ring_tail(std::shared_ptr<std::atomic<size_t>> ar) { ring_tail_sp_ = std::move(ar); }
void AUDIO_RS::set_abs_idx(std::shared_ptr<std::atomic<uint64_t>> ar) { abs_idx_sp_ = std::move(ar); }

void AUDIO_RS::set_i2s_buffer(std::span<uint32_t> i2s_buffer) { i2s_buffer_ = i2s_buffer; }
void AUDIO_RS::set_ring_payload_flat(std::span<uint32_t> flat, size_t frames_per_packet) {
    ring_payload_flat_ = flat;
    frames_per_packet_ = frames_per_packet;
}

// ---- internal helpers ----
void AUDIO_RS::Ring_Clear_Rst() {
    if (ring_payload_flat_.size()) {
        std::fill(ring_payload_flat_.begin(), ring_payload_flat_.end(), 0u);
    }
}

// ---- Trampolines ----
void AUDIO_RS::I2SReaderTrampoline(void* pv) { static_cast<AUDIO_RS*>(pv)->I2SReaderLoop(); }
void AUDIO_RS::RingWriterTrampoline(void* pv) { static_cast<AUDIO_RS*>(pv)->RingWriterLoop(); }
void AUDIO_RS::FingerprintTrampoline(void* pv) { static_cast<AUDIO_RS*>(pv)->FingerprintLoop(); }
void AUDIO_RS::NetworkTrampoline(void* pv) { static_cast<AUDIO_RS*>(pv)->NetworkLoop(); }

// ---- public start/stop ----
bool AUDIO_RS::start_all(uint32_t reader_stack, UBaseType_t reader_prio,
                         uint32_t writer_stack, UBaseType_t writer_prio,
                         uint32_t fp_stack, UBaseType_t fp_prio,
                         uint32_t net_stack, UBaseType_t net_prio,
                         BaseType_t pinned_core)
{
    if (i2s_buffer_.size() == 0 || ring_payload_flat_.size() == 0 || frames_per_packet_ == 0) {
        Serial.println("AUDIO_RS::start_all - buffers not set");
        return false;
    }

    // Create queue to pass bytes_read from reader -> writer
    // item = size_t bytes_read, queue length 8 (adjust as needed)
    i2s_queue_ = xQueueCreate(8, sizeof(size_t));
    if (!i2s_queue_) {
        Serial.println("AUDIO_RS::start_all - failed to create i2s_queue");
        return false;
    }

    // create tasks pinned to pinned_core (or unpinned if pinned_core < 0)
    BaseType_t ok;
    if (pinned_core >= 0)
        ok = xTaskCreatePinnedToCore(I2SReaderTrampoline, "I2SReader", reader_stack, this, reader_prio, &reader_task_, pinned_core);
    else
        ok = xTaskCreate(I2SReaderTrampoline, "I2SReader", reader_stack, this, reader_prio, &reader_task_);
    if (ok != pdPASS) { Serial.println("I2SReader create failed"); return false; }

    if (pinned_core >= 0)
        ok = xTaskCreatePinnedToCore(RingWriterTrampoline, "RingWriter", writer_stack, this, writer_prio, &writer_task_, pinned_core);
    else
        ok = xTaskCreate(RingWriterTrampoline, "RingWriter", writer_stack, this, writer_prio, &writer_task_);
    if (ok != pdPASS) { Serial.println("RingWriter create failed"); return false; }

    // fingerprint and network are optional worker tasks. Create them but it's ok if they fail
    if (pinned_core >= 0)
        ok = xTaskCreatePinnedToCore(FingerprintTrampoline, "Fingerprint", fp_stack, this, fp_prio, &fingerprint_task_, pinned_core);
    else
        ok = xTaskCreate(FingerprintTrampoline, "Fingerprint", fp_stack, this, fp_prio, &fingerprint_task_);
    if (ok != pdPASS) Serial.println("Fingerprint task not created");

    if (pinned_core >= 0)
        ok = xTaskCreatePinnedToCore(NetworkTrampoline, "Network", net_stack, this, net_prio, &network_task_, pinned_core);
    else
        ok = xTaskCreate(NetworkTrampoline, "Network", net_stack, this, net_prio, &network_task_);
    if (ok != pdPASS) Serial.println("Network task not created");

    return true;
}

void AUDIO_RS::stop_all()
{
    if (reader_task_) vTaskDelete(reader_task_);
    if (writer_task_) vTaskDelete(writer_task_);
    if (fingerprint_task_) vTaskDelete(fingerprint_task_);
    if (network_task_) vTaskDelete(network_task_);
    if (i2s_queue_) { vQueueDelete(i2s_queue_); i2s_queue_ = nullptr; }
}

// ---- I2S Reader Loop (only read & push bytes_read into queue) ----
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

// ---- Ring Writer Loop (consume bytes_read and write into ring payload) ----
void AUDIO_RS::RingWriterLoop()
{
    if (i2s_buffer_.size() == 0 || ring_payload_flat_.size() == 0 || frames_per_packet_ == 0 || i2s_queue_ == nullptr) {
        vTaskDelay(pdMS_TO_TICKS(100));
        vTaskDelete(nullptr);
        return;
    }

    // ensure ring flat length is multiple of frames
    if (ring_payload_flat_.size() % frames_per_packet_ != 0) {
        Serial.println("RingWriterLoop - ring size not multiple of frames_per_packet");
        vTaskDelay(pdMS_TO_TICKS(100));
        vTaskDelete(nullptr);
        return;
    }

    const size_t ring_slots = ring_payload_flat_.size() / frames_per_packet_;
    const size_t frames = frames_per_packet_;
    const size_t ring_mask = (ring_slots & (ring_slots - 1)) == 0 ? (ring_slots - 1) : 0; // 0 means not power of two

    for (;;) {
        size_t bytes_read = 0;
        // block until a reader reports bytes_read
        if (xQueueReceive(i2s_queue_, &bytes_read, portMAX_DELAY) != pdTRUE) continue;

        size_t word_count = bytes_read / sizeof(uint32_t);
        size_t available_frames = (word_count >= (frames * DEFAULT_CHANNEL_COUNT)) ? frames : (word_count / DEFAULT_CHANNEL_COUNT);
        if (available_frames > frames) available_frames = frames;

        size_t head = ring_head_sp_ ? ring_head_sp_->load(std::memory_order_relaxed) : 0;
        size_t tail = ring_tail_sp_ ? ring_tail_sp_->load(std::memory_order_acquire) : 0;
        size_t next_head = head + 1;

        if ((next_head - tail) > ring_slots) {
            // ring full -> drop packet (advance absolute index)
            if (abs_idx_sp_) {
                abs_idx_sp_->fetch_add((uint64_t)available_frames, std::memory_order_relaxed);
            }
            // yield to other tasks
            taskYIELD();
            continue;
        }

        // compute slot & slice row (no copy)
        size_t slot = (ring_mask != 0) ? (head & ring_mask) : (head % ring_slots);
        uint32_t* row_ptr = ring_payload_flat_.data() + slot * frames;
        std::span<uint32_t> row(row_ptr, frames);

        // fill the row selecting a channel (same as before: channel index 1)
        if (available_frames == frames) {
            for (size_t i = 0; i < frames; ++i) {
                row[i] = i2s_buffer_[i * DEFAULT_CHANNEL_COUNT + 1];
            }
        } else {
            for (size_t i = 0; i < available_frames; ++i) {
                row[i] = i2s_buffer_[i * DEFAULT_CHANNEL_COUNT + 1];
            }
            // zero-fill remainder
            for (size_t i = available_frames; i < frames; ++i) row[i] = 0;
        }

        // capture sample index & timestamp if available
        uint64_t first_sample_idx = abs_idx_sp_ ? abs_idx_sp_->load(std::memory_order_relaxed) : 0;
        uint64_t ts = (uint64_t)esp_timer_get_time();

        // TODO: write RING_FRAMES[slot], RING_FIRST_INDEX[slot], RING_TIMESTAMP[slot] if those arrays exist
        // Example (if globals exist):
        // RING_FRAMES[slot] = (uint16_t)available_frames;
        // RING_FIRST_INDEX[slot] = first_sample_idx;
        // RING_TIMESTAMP[slot] = ts;

        // publish head and advance absolute sample index
        if (ring_head_sp_) ring_head_sp_->store(next_head, std::memory_order_release);
        if (abs_idx_sp_) abs_idx_sp_->fetch_add((uint64_t)available_frames, std::memory_order_relaxed);

        // optionally notify fingerprint task or network here (e.g., give a semaphore or push slot index onto another queue)
    }
}

// ---- Fingerprint Loop (skeleton) ----
void AUDIO_RS::FingerprintLoop()
{
    // This is a placeholder. Typical job:
    //  - wait for notification a new slot is available (via semaphore/queue)
    //  - slice the row (as in RingWriterLoop) and compute fingerprint/STFT
    //  - push fingerprint/metadata to network queue or send directly
    for (;;) {
        // sleep for a small amount (or block on a queue/semaphore)
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

// ---- Network Loop (skeleton) ----
void AUDIO_RS::NetworkLoop()
{
    // Placeholder: send metadata (abs_sample_index, fingerprint) to remote, or
    // receive metadata and pass it to matcher.
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(200));
    }
}
