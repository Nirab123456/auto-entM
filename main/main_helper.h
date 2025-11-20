#pragma once
#include "headers/audio_read_send.h"   // your AUDIO_RS header (adjust path if needed)
#include "headers/microphoneConfig.h" // your MicrophoneConfig definition (adjust path)
#include "headers/ReciverConfig.h"    // optional, if used for network connect

const char* WIFI_SSID = "94 Pembroke Street - 2";
const char* WiFi_PASS = "welcomehome";


constexpr uint16_t USER_SAMPLE_RATE = 48000;
constexpr uint8_t USER_DMA_BUFFER_COUNT = 6;
constexpr uint8_t USER_PIN_CLK = 7;
constexpr uint8_t USER_PIN_WS = 15;
constexpr uint8_t USER_PIN_SD = 16;
constexpr uint8_t RESET_WIFI_BUTTON_PIN = 1;
constexpr size_t FRAMES_PER_PACKET = 1024;
constexpr size_t RING_SIZE = 64;
static_assert((RING_SIZE & (RING_SIZE - 1)) == 0, "Ring size should be power of 2");
constexpr size_t I2S_WORD_SLOTS_LEN = FRAMES_PER_PACKET * 2;
constexpr size_t RING_FLAT_LEN = RING_SIZE * FRAMES_PER_PACKET;

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

inline void user_mic_config_setter(MicrophoneConfig &mcfg)
{
    mcfg.channel_count = 1;
    mcfg.i2s_port = I2S_NUM_0;
    i2s_std_config_t std_cfg;

    std_cfg.clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(USER_SAMPLE_RATE);
    std_cfg.slot_cfg = I2S_STD_MSB_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_MONO);
    std_cfg.gpio_cfg.mclk = I2S_GPIO_UNUSED,
    std_cfg.gpio_cfg.bclk = static_cast<gpio_num_t>(USER_PIN_CLK);
    std_cfg.gpio_cfg.ws = static_cast<gpio_num_t>(USER_PIN_WS);
    std_cfg.gpio_cfg.din = static_cast<gpio_num_t>(USER_PIN_SD);
    std_cfg.gpio_cfg.invert_flags.mclk_inv = 0;
    std_cfg.gpio_cfg.invert_flags.bclk_inv = 0;
    std_cfg.gpio_cfg.invert_flags.ws_inv = 0;
    mcfg.i2s_configuration = std_cfg;
}