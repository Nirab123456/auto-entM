#pragma once
#include <Arduino.h>
#include <atomic>
#include <memory>
#include <cstdint>
#include "driver/i2s.h"


class AUDIO_RS
{
    private:
        std::shared_ptr <std::atomic<bool>> Consumer_ready_ar_{nullptr};
        std::shared_ptr <std::atomic<size_t>> Ring_head_ar_{nullptr};
        std::shared_ptr <std::atomic<size_t>> Ring_tail_ar_{nullptr};


        uint32_t* buffer_ptr_ = nullptr;
        size_t buffer_len_ = 0;

        void Ring_Clear_Rst();
    public:
        AUDIO_RS();
        explicit AUDIO_RS(uint32_t* buffer, size_t length);
        AUDIO_RS(
            uint32_t* buffer,
            size_t length,
            std::shared_ptr<std::atomic<bool>>      consumer_ready,
            std::shared_ptr<std::atomic<size_t>>    ring_head,
            std::shared_ptr<std::atomic<size_t>>    ring_tail




        );

        void set_consumer_ready(std::shared_ptr<std::atomic<bool>>ar);
        void set_ring_head(std::shared_ptr<std::atomic<size_t>>ar);
        void set_ring_tail(std::shared_ptr<std::atomic<size_t>>ar);


        //accessor
        uint32_t* buffer() const;
        size_t buffer_len() const;
        bool has_consumer_ready() const;

        void Audio_Task(void* pv);


};