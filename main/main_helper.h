#pragma once
#include "headers/audio_read_send.h"   // your AUDIO_RS header (adjust path if needed)
#include "headers/microphoneConfig.h" // your MicrophoneConfig definition (adjust path)
#include "headers/ReciverConfig.h"    // optional, if used for network connect

constexpr uint16_t USER_SAMPLE_RATE = 48000;
constexpr uint8_t USER_DMA_BUFFER_COUNT = 6;
constexpr uint8_t USER_PIN_CLK = 7;
constexpr uint8_t USER_PIN_WS = 15;
constexpr uint8_t USER_PIN_SD = 16;


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
inline void user_mic_config_setter(MicrophoneConfig& mcfg)
{
    mcfg.channel_count = 1;
    mcfg.i2s_port = I2S_NUM_0;
    mcfg.i2s_configuration = {
        .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX),
        .sample_rate = USER_SAMPLE_RATE,
        .bits_per_sample = I2S_BITS_PER_SAMPLE_32BIT,
        .channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT,
        .communication_format = (i2s_comm_format_t) (I2S_COMM_FORMAT_I2S | I2S_COMM_FORMAT_I2S_MSB),
        .intr_alloc_flags = 0,
        .dma_buf_count = USER_DMA_BUFFER_COUNT,
        .dma_buf_len = FRAMES_PER_PACKET / 2,
        .use_apll = true,
        .tx_desc_auto_clear = false,
        .fixed_mclk = 0
    };
    mcfg.i2spinconfiguration = {
        .bck_io_num = USER_PIN_CLK,
        .ws_io_num = USER_PIN_WS,
        .data_out_num = I2S_PIN_NO_CHANGE,
        .data_in_num = USER_PIN_SD
    };

}
