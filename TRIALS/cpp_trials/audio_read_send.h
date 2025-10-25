#pragma once
#include <Arduino.h>
#include <span>
#include <atomic>
#include <memory>
#include <cstdint>
#include <cstddef>
#include "task_prio_core_stack.h"

class AUDIO_RS {
private:
    // shared state (optional; nullptr if not provided)
    std::shared_ptr<std::atomic<bool>>      consumer_ready_sp_{nullptr};
    std::shared_ptr<std::atomic<size_t>>    ring_head_sp_{nullptr};
    std::shared_ptr<std::atomic<size_t>>    ring_tail_sp_{nullptr};
    std::shared_ptr<std::atomic<uint64_t>>  abs_idx_sp_{nullptr};

    // non-owning views (no copy)
    std::span<uint32_t> i2s_buffer_{};
    std::span<uint32_t> ring_payload_flat_{}; // flat view: RING_SIZE * FRAMES_PER_PACKET
    size_t frames_per_packet_{0};

    void Ring_Clear_Rst();

    // core loop that runs forever (instance method)
    void AudioTaskLoop();

public:
    AUDIO_RS() = default;

    // Main constructor: pass i2s buffer span + flat ring payload span + frames per row
    AUDIO_RS(std::span<uint32_t> i2s_buffer,
            std::span<uint32_t> ring_payload_flat,
            size_t frames_per_packet,
            std::shared_ptr<std::atomic<bool>> consumer_ready = nullptr,
            std::shared_ptr<std::atomic<size_t>> ring_head = nullptr,
            std::shared_ptr<std::atomic<size_t>> ring_tail = nullptr,
            std::shared_ptr<std::atomic<uint64_t>> abs_idx = nullptr);

    // setters if you want to set later
    void set_consumer_ready(std::shared_ptr<std::atomic<bool>> ar);
    void set_ring_head(std::shared_ptr<std::atomic<size_t>> ar);
    void set_ring_tail(std::shared_ptr<std::atomic<size_t>> ar);
    void set_abs_idx(std::shared_ptr<std::atomic<uint64_t>> ar);

    void set_i2s_buffer(std::span<uint32_t> i2s_buffer);
    void set_ring_payload_flat(std::span<uint32_t> flat, size_t frames_per_packet);

    // accessors
    std::span<uint32_t> i2s_buffer() const { return i2s_buffer_; }
    std::span<uint32_t> ring_payload_flat() const { return ring_payload_flat_; }
    size_t frames_per_packet() const { return frames_per_packet_; }
    bool has_consumer_ready() const { return static_cast<bool>(consumer_ready_sp_); }

    // FreeRTOS task trampoline: pass pointer-to-instance as pv
    static void TaskTrampoline(void* pv);
    // public method to spawn task easily (optional)
    bool start_task(const char* name = "AudioTask", uint32_t stack = 4096, UBaseType_t prio = 1, BaseType_t core = 1);
};
