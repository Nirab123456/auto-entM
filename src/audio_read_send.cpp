#include "audio_read_send.h"
#include "audio_conf_sett.h"

// default ctor
AUDIO_RS::AUDIO_RS() = default;

// buffer-only ctor
AUDIO_RS::AUDIO_RS(uint32_t* buffer, size_t length)
  : buffer_ptr_(buffer), buffer_len_(length) {}

// full ctor: accept shared_ptrs by value and move them into members
AUDIO_RS::AUDIO_RS(uint32_t* buffer, size_t length,
                   std::shared_ptr<std::atomic<bool>>    consumer_ready,
                   std::shared_ptr<std::atomic<size_t>>  ring_head,
                   std::shared_ptr<std::atomic<size_t>>  ring_tail,
                   std::shared_ptr<std::atomic<uint32_t>> sequence_counter)
  : buffer_ptr_(buffer),
    buffer_len_(length),
    consumer_ready_sp_(std::move(consumer_ready)),
    ring_head_sp_(std::move(ring_head)),
    ring_tail_sp_(std::move(ring_tail)),
    sequence_counter_sp_(std::move(sequence_counter))
{}

// setters (allow adding later)
void AUDIO_RS::set_consumer_ready(std::shared_ptr<std::atomic<bool>> sp) {
    consumer_ready_sp_ = std::move(sp);
}
void AUDIO_RS::set_ring_head(std::shared_ptr<std::atomic<size_t>> sp) {
    ring_head_sp_ = std::move(sp);
}
void AUDIO_RS::set_ring_tail(std::shared_ptr<std::atomic<size_t>> sp) {
    ring_tail_sp_ = std::move(sp);
}
void AUDIO_RS::set_sequence_counter(std::shared_ptr<std::atomic<uint32_t>> sp) {
    sequence_counter_sp_ = std::move(sp);
}

// accessors
uint32_t* AUDIO_RS::buffer() const { return buffer_ptr_; }
size_t AUDIO_RS::buffer_len() const { return buffer_len_; }
bool AUDIO_RS::has_consumer_ready() const { return static_cast<bool>(consumer_ready_sp_); }

// internal helper
void AUDIO_RS::Ring_Clear_Rst() {
    if (!buffer_ptr_ || buffer_len_ == 0) return;
    for (size_t i = 0; i < buffer_len_; ++i) buffer_ptr_[i] = 0;
}

// main task example
void AUDIO_RS::Audio_Task(void* pv) {
    if (!buffer_ptr_ || buffer_len_ == 0) {
        vTaskDelay(pdMS_TO_TICKS(10));
        return;
    }

    // Example usage of ring_head/ tail / sequence_counter if present
    if (ring_head_sp_ && ring_tail_sp_) {
        size_t head = ring_head_sp_->load(std::memory_order_acquire);
        size_t tail = ring_tail_sp_->load(std::memory_order_acquire);
        // do something with head/tail (bound checks etc.)
        (void)head; (void)tail;
    }

    // sequence counter example
    if (sequence_counter_sp_) {
        sequence_counter_sp_->fetch_add(1, std::memory_order_relaxed);
    }

    // consumer_ready example
    if (consumer_ready_sp_) {
        // example: check flag; if true, proceed and then clear it
        if (consumer_ready_sp_->load(std::memory_order_acquire)) {
            // produce data into buffer...
            consumer_ready_sp_->store(false, std::memory_order_release);
        } else {
            // wait a little
            vTaskDelay(pdMS_TO_TICKS(5));
        }
    }
}
