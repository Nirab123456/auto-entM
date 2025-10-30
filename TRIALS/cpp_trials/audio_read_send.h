#pragma once 
#include <Arduino.h>
#include <span>
#include <atomic>
#include <memory>
#include <cstdint>
#include "task_prio_core_stack.h"

using TASK_TRAMPOLINE_FN = void(*)(void*);

class AUDIO_RS 
{
    private:
        std::shared_ptr<std::atomic<bool>>          consumer_ready_sp_{nullptr};
        std::shared_ptr<std::atomic<size_t>>        ring_head_sp_{nullptr};
        std::shared_ptr<std::atomic<size_t>>        ring_tail_sp_{nullptr};
        std::shared_ptr<std::atomic<uint64_t>>      abs_idx_sp_{nullptr};
        

        enum class OverRunPolicy : uint8_t {
            DROP_NEWEST = 0,
            DROP_OLDEST = 1
        };

        OverRunPolicy  overrun_policy_ = OverRunPolicy::DROP_NEWEST;

        std::atomic<uint32_t> drop_count_newest_{0};
        std::atomic<uint32_t> drop_count_oldest_{0};

        //non-owning 
        std::span<uint32_t> i2s_buffer_{};
        std::span<uint32_t> ring_payload_flat_{};
        size_t frames_per_packet_{0};


        //rtos premetives
        QueueHandle_t       i2s_queue_{nullptr};
        TaskHandle_t        read_task_{nullptr};
        TaskHandle_t        write_task_{nullptr};
        TaskHandle_t        fingerprint_task_{nullptr};
        TaskHandle_t        networktask_{nullptr};

        void I2SReaderLoop();
        void RingWriterLoop();
        void FingerPrintLoop();
        void NetworkTaskLoop();
        


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
        void Ring_clear_Rst();

        void set_consumer_ready(std::shared_ptr<std::atomic<bool>> ar);
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
        static void AudioTaskTrampoline(void* pv);
        static void I2SReadTrampoline(void* pv);
        static void RingWriterFRMI2STrampoline(void* pv);
        static void NetworkTaskTrampoline(void* pv);
        bool start_task(
            const char* name = AUDIOTASK, 
            uint32_t stack = AUDIOTASK_STACK, 
            UBaseType_t prio = AUDIOTASK_PRIORITY, 
            BaseType_t core = AUDIOTASK_CORE,
            TASK_TRAMPOLINE_FN trampoline = AudioTaskTrampoline,
            void* arg
        );

        

        // set policy directly
        void set_overrun_policy(OverRunPolicy p) { overrun_policy_ = p; }

        // convenience methods (you asked for these names)
        void ovverrunpolicy_newest() { set_overrun_policy(OverRunPolicy::DROP_NEWEST); }
        void ovverrunpolicy_oldest() { set_overrun_policy(OverRunPolicy::DROP_OLDEST); }

        // read-only stats
        uint32_t get_drop_count_newest() const { return drop_count_newest_.load(std::memory_order_relaxed); }
        uint32_t get_drop_count_oldest() const { return drop_count_oldest_.load(std::memory_order_relaxed); }

};