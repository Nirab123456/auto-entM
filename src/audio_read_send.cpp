#include "audio_read_send.h"

// ---- ctors ----
AUDIO_RS::AUDIO_RS()
  : consumer_ready_sp_(nullptr),
    buffer_ptr_(nullptr),
    buffer_len_(0) {}

AUDIO_RS::AUDIO_RS(uint32_t* buffer, size_t length)
  : consumer_ready_sp_(nullptr),
    buffer_ptr_(buffer),
    buffer_len_(length) {}

AUDIO_RS::AUDIO_RS(std::shared_ptr<std::atomic<bool>> sp_consumer_ready)
  : consumer_ready_sp_(std::move(sp_consumer_ready)),
    buffer_ptr_(nullptr),
    buffer_len_(0) {}

AUDIO_RS::AUDIO_RS(uint32_t* buffer, size_t length,
                   std::shared_ptr<std::atomic<bool>> sp_consumer_ready)
  : consumer_ready_sp_(std::move(sp_consumer_ready)),
    buffer_ptr_(buffer),
    buffer_len_(length) {}

// ---- accessors ----
uint32_t* AUDIO_RS::buffer() const { return buffer_ptr_; }
size_t AUDIO_RS::buffer_len() const { return buffer_len_; }
bool AUDIO_RS::has_consumer_ready() const { return static_cast<bool>(consumer_ready_sp_); }

// ---- helper ----
void AUDIO_RS::RING_Clear_Rst() {
    // Example: zero the buffer (non-owning, so we only write into it)
    if (!buffer_ptr_ || buffer_len_ == 0) return;
    for (size_t i = 0; i < buffer_len_; ++i) buffer_ptr_[i] = 0;
    // If you had head/tail counters in this class, reset them here.
}

// ---- main task ----
void AUDIO_RS::Audio_TASK(void* pv) {
    // Example pseudo-task: check flag & write sample counters
    if (!buffer_ptr_ || buffer_len_ == 0) {
        // nothing to do
        vTaskDelay(pdMS_TO_TICKS(10));
        return;
    }

    // If there's a shared atomic flag, check it safely:
    if (consumer_ready_sp_) {
        bool ready = consumer_ready_sp_->load(std::memory_order_acquire);
        if (!ready) {
            // consumer is not ready — maybe wait a bit
            vTaskDelay(pdMS_TO_TICKS(10));
            return;
        }
    }


    // Suspend or delay to simulate periodic task
    vTaskDelay(pdMS_TO_TICKS(20));
}
