#pragma once
#include <Arduino.h>
#include <atomic>
#include <memory>
#include <cstddef>
#include <cstdint>

class AUDIO_RS {
private:
    // shared ownership of the atomic flag (may be nullptr)
    std::shared_ptr<std::atomic<bool>> consumer_ready_sp_;

    // pointer to external buffer (non-owning)
    uint32_t* buffer_ptr_ = nullptr;
    size_t buffer_len_ = 0;   // length in elements

    // helper to clear ring/reset indexes (implementation in .cpp)
    void RING_Clear_Rst();

public:
    // ctors & dtor
    AUDIO_RS(); // default
    explicit AUDIO_RS(uint32_t* buffer, size_t length);
    explicit AUDIO_RS(std::shared_ptr<std::atomic<bool>> sp_consumer_ready);
    AUDIO_RS(uint32_t* buffer, size_t length,
             std::shared_ptr<std::atomic<bool>> sp_consumer_ready);

    // accessors
    uint32_t* buffer() const;
    size_t buffer_len() const;
    bool has_consumer_ready() const;

    // the main task function to be called in a FreeRTOS task
    void Audio_TASK(void* pv);
};
