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

    // zero the config then assign fields (C++-safe)
    i2s_config_t cfg;
    ::memset(&cfg, 0, sizeof(cfg));   // <-- use global memset

    cfg.mode = static_cast<i2s_mode_t>(I2S_MODE_MASTER | I2S_MODE_RX);
    cfg.sample_rate = USER_SAMPLE_RATE;
    cfg.bits_per_sample = I2S_BITS_PER_SAMPLE_32BIT;
    cfg.channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT;

    // Use standard constant if available; fallback to legacy combo
    #ifdef I2S_COMM_FORMAT_STAND_I2S
    cfg.communication_format = I2S_COMM_FORMAT_STAND_I2S;
    #else
    cfg.communication_format = static_cast<i2s_comm_format_t>(I2S_COMM_FORMAT_I2S | I2S_COMM_FORMAT_I2S_MSB);
    #endif

    cfg.intr_alloc_flags = 0;
    // legacy fields may be marked deprecated but still usable for now
    cfg.dma_buf_count = USER_DMA_BUFFER_COUNT;
    cfg.dma_buf_len = (FRAMES_PER_PACKET / 2);

    cfg.use_apll = true;
    cfg.tx_desc_auto_clear = false;
    cfg.fixed_mclk = 0;

    mcfg.i2s_configuration = cfg;

    // pins: zero then set explicitly (set mck_io_num to avoid missing-field warning)
    i2s_pin_config_t pins;
    ::memset(&pins, 0, sizeof(pins));

    pins.bck_io_num = USER_PIN_CLK;
    pins.ws_io_num  = USER_PIN_WS;
    pins.data_out_num = I2S_PIN_NO_CHANGE;
    pins.data_in_num = USER_PIN_SD;
    pins.mck_io_num = I2S_PIN_NO_CHANGE; // explicit

    mcfg.i2spinconfiguration = pins;
}