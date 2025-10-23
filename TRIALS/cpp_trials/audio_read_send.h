#pragma once
#include <Arduino.h>
#include <atomic>
#include <memory>
#include <cstddef>
#include <cstdint>
#include "driver/i2s.h"

class AUDIO_RS {
private:
    // shared ownership of atomics (optional; default nullptr)
    std::shared_ptr<std::atomic<bool>>    consumer_ready_sp_{nullptr};
    std::shared_ptr<std::atomic<size_t>>  ring_head_sp_{nullptr};
    std::shared_ptr<std::atomic<size_t>>  ring_tail_sp_{nullptr};
    std::shared_ptr<std::atomic<uint32_t>> sequence_counter_sp_{nullptr}; // example added later

    // non-owning buffer pointer + length
    uint32_t* buffer_ptr_ = nullptr;
    size_t buffer_len_ = 0;

    void Ring_Clear_Rst();

public:
    AUDIO_RS();
    explicit AUDIO_RS(uint32_t* buffer, size_t length);

    // single constructor that accepts optional shared_ptrs (default nullptr)
    AUDIO_RS(uint32_t* buffer, size_t length,
             std::shared_ptr<std::atomic<bool>>    consumer_ready,
             std::shared_ptr<std::atomic<size_t>>  ring_head,
             std::shared_ptr<std::atomic<size_t>>  ring_tail,
             std::shared_ptr<std::atomic<uint32_t>> sequence_counter = nullptr);

    // setters if you want to add atomics later after construction
    void set_consumer_ready(std::shared_ptr<std::atomic<bool>> sp);
    void set_ring_head(std::shared_ptr<std::atomic<size_t>> sp);
    void set_ring_tail(std::shared_ptr<std::atomic<size_t>> sp);
    void set_sequence_counter(std::shared_ptr<std::atomic<uint32_t>> sp);

    // accessors
    uint32_t* buffer() const;
    size_t buffer_len() const;
    bool has_consumer_ready() const;

    // main method
    void Audio_Task(void* pv);
};
