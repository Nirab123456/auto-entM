#pragma once 
#include <Arduino.h>
#include <span>
#include <atomic>
#include <memory>
#include <cstdint>
#include "task_prio_core_stack.h"


class AUDIO_RS 
{
    private:
        std::shared_ptr<std::atomic<bool>>          consumer_ready_sp_{nullptr};
        std::shared_ptr<std::atomic<size_t>>        ring_head_sp_{nullptr};
        std::shared_ptr<std::atomic<size_t>>        ring_tail_sp_{nullptr};
        std::shared_ptr<std::atomic<uint64_t>>      abs_idx_sp{nullptr};

        //non-owning 
        std::span<uint32_t> i2s_buffer_{};
        std::span<uint32_t> ring_payload_flat_{};
        size_t frames_per_packet_{0};

        void Ring_clear_Rst();
        void AudioTaskLoop();
    public:
        AUDIO_RS() = default;

        AUDIO_RS(
            std::span<uint32_t> i2s_buffer,
            std::span<uint32_t> ring_payload_flat,
            size_t frames_per_packet,
            std::shared_ptr<std::atomic<bool>> consumer_ready = nullptr,
            std::shared_ptr<std::atomic<size_t>> ring_head = nullptr,
            std::shared_ptr<std::atomic<size_t>> ring_tail = nullptr,
            std::shared_ptr<std::atomic<uint64_t>> abs_idx = nullptr
        );

        void set_consumer_ready(std::atomic<std::atomic<bool>> ar);
        void set_ring_head(std::shared_ptr<std::atomic<size_t>> ar);
        void set_ring_tail(std::shared_ptr<std::atomic<size_t>> ar);
        void set_abs_idx(std::shared_ptr<std::atomic<uint64_t>> ar);
        
        void set_i2s_buffer(std::span<uint32_t> i2s_buffer);
        void set_ring_payload_flat(std::span<uint32_t>flat, size_t frames_per_packet);


        std::span<uint32_t> i2s_buffer() const
        {
            return i2s_buffer_;
        }
        std::span<uint32_t> ring_payload_flat() const
        {
            return ring_payload_flat_;
        }
        size_t frames_per_packet() const
        {
            return frames_per_packet_;
        }
        static void TaskTrampoline(void* pv);
        bool start_task(const char* name = AUDIOTASK, uint32_t stack = AUDIOTASK_STACK, UBaseType_t prio = AUDIOTASK_PRIORITY, BaseType_t core = AUDIOTASK_CORE);

};