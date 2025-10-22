#pragma once
#include <Arduino.h>
#include <atomic>
#include <memory>
#include <cstddef>
#include <cstdint>
#include "driver/i2s.h"

class AUDIO_RS {
private:
    std::shared_ptr<std::atomic<bool>> Consumer_Ready_SP_;
    uint32_t* buffer_ptr_ = nullptr;
    size_t buffer_len_ = 0;
    void Ring_Clear_Rst();

public:
    AUDIO_RS();
    explicit AUDIO_RS(uint32_t* buffer, size_t length);
    explicit AUDIO_RS(std::shared_ptr<std::atomic<bool>>SP_consumer_ready);
    AUDIO_RS(uint32_t* buffer, 
        size_t length,
        std::shared_ptr<std::atomic<bool>>SP_consumer_ready
    );
    uint32_t* buffer() const;
    size_t buffer_len() const;
    bool has_consumer_ready() const;
    void Audio_Task(void* pv);
};
