#pragma once
#include <Arduino.h>
#include <span>
#include <atomic>
#include <memory>
#include <cstdint>
#include <cstddef>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"

// Forward: make sure FRAMES_PER_PACKET, BYTES_PER_SAMPLE, DEFAULT_CHANNEL_COUNT,
// BYTES_TO_READ, etc. are defined in your project (audio_conf_sett.h).

using TASK_TRAMPOLINE_FN = TaskFunction_t;

class AUDIO_RS {
private:
    // Shared state / atomics
    std::shared_ptr<std::atomic<bool>>      consumer_ready_sp_{nullptr};
    std::shared_ptr<std::atomic<size_t>>    ring_head_sp_{nullptr};
    std::shared_ptr<std::atomic<size_t>>    ring_tail_sp_{nullptr};
    std::shared_ptr<std::atomic<uint64_t>>  abs_idx_sp_{nullptr};

    // Buffers (non-owning spans)
    std::span<uint32_t> i2s_buffer_{};
    std::span<uint32_t> ring_payload_flat_{}; // flat: RING_SLOTS * frames_per_packet_
    size_t frames_per_packet_{0};

    // RTOS primitives for decoupling
    QueueHandle_t    i2s_queue_{nullptr};   // carries size_t bytes_read items
    TaskHandle_t     reader_task_{nullptr};
    TaskHandle_t     writer_task_{nullptr};
    TaskHandle_t     fingerprint_task_{nullptr};
    TaskHandle_t     network_task_{nullptr};

    // Internal helpers
    void Ring_Clear_Rst();

    // Task loops (instance methods)
    void I2SReaderLoop();    // reads from i2s and pushes bytes_read to queue
    void RingWriterLoop();   // pops bytes_read and writes into ring payload
    void FingerprintLoop();  // placeholder: compute fingerprint & push to network
    void NetworkLoop();      // placeholder: send/recv metadata

    // Trampolines for FreeRTOS
    static void I2SReaderTrampoline(void* pv);
    static void RingWriterTrampoline(void* pv);
    static void FingerprintTrampoline(void* pv);
    static void NetworkTrampoline(void* pv);

public:
    AUDIO_RS() = default;

    // Main constructor
    AUDIO_RS(std::span<uint32_t> i2s_buffer,
            std::span<uint32_t> ring_payload_flat,
            size_t frames_per_packet,
            std::shared_ptr<std::atomic<bool>> consumer_ready = nullptr,
            std::shared_ptr<std::atomic<size_t>> ring_head = nullptr,
            std::shared_ptr<std::atomic<size_t>> ring_tail = nullptr,
            std::shared_ptr<std::atomic<uint64_t>> abs_idx = nullptr);

    // setters
    void set_consumer_ready(std::shared_ptr<std::atomic<bool>> ar);
    void set_ring_head(std::shared_ptr<std::atomic<size_t>> ar);
    void set_ring_tail(std::shared_ptr<std::atomic<size_t>> ar);
    void set_abs_idx(std::shared_ptr<std::atomic<uint64_t>> ar);

    void set_i2s_buffer(std::span<uint32_t> i2s_buffer);
    void set_ring_payload_flat(std::span<uint32_t> flat, size_t frames_per_packet);

    // Create and start modular tasks. Customize stack/prio as needed.
    bool start_all(uint32_t reader_stack = 4096, UBaseType_t reader_prio = 3,
                   uint32_t writer_stack = 8192, UBaseType_t writer_prio = 2,
                   uint32_t fp_stack = 8192, UBaseType_t fp_prio = 1,
                   uint32_t net_stack = 4096, UBaseType_t net_prio = 1,
                   BaseType_t pinned_core = 1);

    // Stop/cleanup (simple)
    void stop_all();

    // Accessors
    std::span<uint32_t> i2s_buffer() const { return i2s_buffer_; }
    std::span<uint32_t> ring_payload_flat() const { return ring_payload_flat_; }
    size_t frames_per_packet() const { return frames_per_packet_; }
};
