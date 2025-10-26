#pragma once
#include <Arduino.h>

inline constexpr uint8_t NUMBERS_OF_CHANNELS = 1;
inline constexpr uint8_t BYTES_PER_SAMPLE   = 4;
inline constexpr uint16_t FRAMES_PER_PACKET = 1024;
// inline constexpr size_t BYTES_TO_READ       = ((size_t)(FRAMES_PER_PACKET * BYTES_PER_SAMPLE * 2));
// inline constexpr size_t PAYLOAD_BYTES = ((size_t)FRAMES_PER_PACKET * BYTES_PER_SAMPLE * NUMBERS_OF_CHANNELS);
// inline constexpr size_t NEEDED_WORDS = ((size_t)FRAMES_PER_PACKET*2);
inline constexpr uint8_t DEFAULT_CHANNEL_COUNT = 2;
inline constexpr size_t RING_SIZE = 64;