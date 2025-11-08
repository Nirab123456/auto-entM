#pragma once
#include "headers/audio_read_send.h"   // your AUDIO_RS header (adjust path if needed)
#include "headers/microphoneConfig.h" // your MicrophoneConfig definition (adjust path)
#include "headers/ReciverConfig.h"    // optional, if used for network connect

constexpr uint16_t USER_SAMPLE_RATE = 48000;
constexpr uint8_t USER_DMA_BUFFER_COUNT = 6;
constexpr uint8_t USER_PIN_CLK = 7;
constexpr uint8_t USER_PIN_WS = 15;
constexpr uint8_t USER_PIN_SD = 16;
constexpr size_t FRAMES_PER_PACKET = 1024;
constexpr size_t RING_SIZE = 64;
static_assert((RING_SIZE & (RING_SIZE - 1)) == 0, "Ring size should be power of 2");
constexpr size_t I2S_WORD_SLOTS_LEN = FRAMES_PER_PACKET * 2;
constexpr size_t RING_FLAT_LEN = RING_SIZE * FRAMES_PER_PACKET;

static uint32_t I2S_WORD_SLOTS[I2S_WORD_SLOTS_LEN];
static uint32_t RING_PAYLOAD[RING_SIZE][FRAMES_PER_PACKET];


static uint16_t RING_FRAMES[RING_SIZE];
static uint64_t Ring_FIRST_INDEX[RING_SIZE];
static uint64_t RING_TIMESTAMP[RING_SIZE];
inline static std::span<uint16_t> ring_frames_span(RING_FRAMES, RING_SIZE);
inline static std::span<uint64_t> ring_first_index_span(Ring_FIRST_INDEX, RING_SIZE);
inline static std::span<uint64_t> ring_ts_us_span(RING_TIMESTAMP, RING_SIZE);


inline std::span<uint32_t> make_ring_flat_span()
{
    return std::span<uint32_t>(&RING_PAYLOAD[0][0], RING_FLAT_LEN);
}

inline auto make_shared_atomic_bool(bool i = false)
{
    return std::make_shared<std::atomic<bool>>(i);
}

inline auto make_shared_atomic_size_t(size_t i)
{
    return std::make_shared<std::atomic<size_t>>(i);
}

inline auto make_shared_atomic_uint64_t(uint64_t i)
{
    return std::make_shared<std::atomic<uint64_t>>(i);
}
inline auto make_shared_atomic_uint32_t(uint32_t i)
{
    return std::make_shared<std::atomic<uint32_t>>(i);
}
