#include "audio_read_send.h"
#include <esp_timer.h>
#include "driver/i2s.h"

// Constructor
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
    abs_idx_sp_(std::move(abs_idx))
{}

// setters
void AUDIO_RS::set_consumer_ready(std::shared_ptr<std::atomic<bool>> ar) { consumer_ready_sp_ = std::move(ar); }
void AUDIO_RS::set_ring_head(std::shared_ptr<std::atomic<size_t>> ar) { ring_head_sp_ = std::move(ar); }
void AUDIO_RS::set_ring_tail(std::shared_ptr<std::atomic<size_t>> ar) { ring_tail_sp_ = std::move(ar); }
void AUDIO_RS::set_abs_idx(std::shared_ptr<std::atomic<uint64_t>> ar) { abs_idx_sp_ = std::move(ar); }

void AUDIO_RS::set_i2s_buffer(std::span<uint32_t> i2s_buffer) { i2s_buffer_ = i2s_buffer; }
void AUDIO_RS::set_ring_payload_flat(std::span<uint32_t> flat, size_t frames_per_packet) {
    ring_payload_flat_ = flat;
    frames_per_packet_ = frames_per_packet;
}

void AUDIO_RS::Ring_Clear_Rst() {
    if (ring_payload_flat_.size()) {
        std::fill(ring_payload_flat_.begin(), ring_payload_flat_.end(), 0u);
    }
}

// Trampoline to create FreeRTOS task from class instance
void AUDIO_RS::TaskTrampoline(void* pv) {
    AUDIO_RS* self = static_cast<AUDIO_RS*>(pv);
    if (!self) {
        vTaskDelete(nullptr);
        return;
    }
    self->AudioTaskLoop();
}

// convenience starter
bool AUDIO_RS::start_task(const char* name, uint32_t stack, UBaseType_t prio, BaseType_t core) {
    BaseType_t ok = xTaskCreatePinnedToCore(
        AUDIO_RS::TaskTrampoline,
        name,
        stack,
        this,       // pv -> pointer to instance
        prio,
        nullptr,
        core
    );
    return ok == pdPASS;
}

// The actual audio loop (converted from your blueprint). It never returns.
void AUDIO_RS::AudioTaskLoop() {
    // Preconditions: we must have buffers and frames_per_packet set
    if (i2s_buffer_.size() == 0 || ring_payload_flat_.size() == 0 || frames_per_packet_ == 0) {
        // nothing to do; bail out (or sleep)
        vTaskDelay(pdMS_TO_TICKS(100));
        vTaskDelete(nullptr);
        return;
    }

    // ring must be integral number of slots
    if (ring_payload_flat_.size() % frames_per_packet_ != 0) {
        Serial.println("AUDIO_RS::AudioTaskLoop - ring payload size not divisible by frames_per_packet");
        vTaskDelay(pdMS_TO_TICKS(100));
        vTaskDelete(nullptr);
        return;
    }

    const size_t ring_slots = ring_payload_flat_.size() / frames_per_packet_;
    const size_t frames = frames_per_packet_;

    Serial.printf("TASK : AUDIOTASK started, frames=%u ring_slots=%u\n", (unsigned)frames, (unsigned)ring_slots);

    bool paused = false;

    for (;;) {
        // check consumer: if not ready -> pause (note: use !consumer_ready)
        if (consumer_ready_sp_) {
            if (!consumer_ready_sp_->load(std::memory_order_acquire)) {
                if (!paused) {
                    Serial.println("AUDIO_RS::AudioTaskLoop - No receiver connected, pausing");
                    i2s_zero_dma_buffer(I2S_NUM_0);
                    paused = true;
                }
                vTaskDelay(pdMS_TO_TICKS(200));
                continue; // keep waiting for receiver
            } else {
                if (paused) {
                    Serial.println("AUDIO_RS::AudioTaskLoop - Resuming");
                    paused = false;
                }
            }
        }

        // read from i2s into the i2s_buffer_ (bytes)
        size_t bytes_read = 0;
        esp_err_t err = i2s_read(
            I2S_NUM_0,
            i2s_buffer_.data(),
            i2s_buffer_.size() * sizeof(uint32_t),
            &bytes_read,
            portMAX_DELAY
        );

        if (err != ESP_OK || bytes_read == 0) {
            // read error or nothing read -> small delay and retry
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        // compute available frames (adapt to your channel logic)
        size_t word_count = bytes_read / sizeof(uint32_t); // number of 32-bit words read
        // YOUR previous logic: available_frames = (word_count >= NEEDED_WORDS) ? FRAMES_PER_PACKET : (word_count / DEFAULT_CHANNEL_COUNT)
        // Here we make an assumption: 2 words per frame (stereo) => adjust to your configuration
        size_t available_frames = (word_count >= (frames * 2)) ? frames : (word_count / 2);
        if (available_frames > frames) available_frames = frames;

        // read head/tail indices
        size_t head = ring_head_sp_ ? ring_head_sp_->load(std::memory_order_relaxed) : 0;
        size_t tail = ring_tail_sp_ ? ring_tail_sp_->load(std::memory_order_acquire) : 0;
        size_t next_head = head + 1;

        // check space
        if ((next_head - tail) > ring_slots) {
            // ring full -> drop packet and advance absolute index
            if (abs_idx_sp_) abs_idx_sp_->fetch_add((uint64_t)available_frames, std::memory_order_relaxed);
            static unsigned drop_count = 0;
            if ((++drop_count % 10) == 0) {
                Serial.printf("AUDIO RING: BUFFER FULL, head-tail=%u free heap=%u\n", (unsigned)(head - tail), (unsigned)esp_get_free_heap_size());
            }
            taskYIELD();
            continue;
        }

        // compute slot and slice row (no copy)
        size_t slot = head & (ring_slots - 1); // works when ring_slots is power-of-two
        uint32_t* row_ptr = ring_payload_flat_.data() + slot * frames;
        std::span<uint32_t> row(row_ptr, frames);

        // copy into row selecting channel (based on your original I2S_WORD_SLOTS logic)
        // i2s_buffer_ holds words; e.g. take i*2+1 for one channel
        if (available_frames == frames) {
            for (size_t i = 0; i < frames; ++i) {
                row[i] = i2s_buffer_[i * 2 + 1];
            }
        } else {
            for (size_t i = 0; i < available_frames; ++i) {
                row[i] = i2s_buffer_[i * 2 + 1];
            }
            // zero-fill rest
            for (size_t i = available_frames; i < frames; ++i) row[i] = 0;
        }

        // record metadata (if you have these arrays as globals, write them here);
        // e.g. RING_FRAMES[slot] = (uint16_t)available_frames; ...
        uint64_t first_s_index = abs_idx_sp_ ? abs_idx_sp_->load(std::memory_order_relaxed) : 0;
        uint64_t ts = (uint64_t)esp_timer_get_time();

        // (You must write to RING_FRAMES, RING_FIRST_INDEX, RING_TIMESTAMP globals here if used)
        // advance head (publish to consumers)
        if (ring_head_sp_) ring_head_sp_->store(next_head, std::memory_order_release);
        if (abs_idx_sp_) abs_idx_sp_->fetch_add((uint64_t)available_frames, std::memory_order_relaxed);

        taskYIELD();
    } // end for loop
}
