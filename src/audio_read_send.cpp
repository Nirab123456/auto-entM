#include "audio_read_send.h"

AUDIO_RS::AUDIO_RS()
    :
    consumer_ready_sp_(nullptr),
    buffer_ptr_(nullptr),
    buffer_len_(0)
{}

AUDIO_RS::AUDIO_RS(uint32_t* buffer, size_t length)
    :
    consumer_ready_sp_(nullptr),
    buffer_ptr_(buffer),
    buffer_len_(length)
{}

AUDIO_RS::AUDIO_RS(std::shared_ptr<std::atomic<bool>> SP_consumer_ready)
    :
    consumer_ready_sp_(std::move(SP_consumer_ready)),
    buffer_ptr_(nullptr),
    buffer_len_(0)
{}
