#pragma once 
#include <span>
#include <atomic>
#include <memory>
#include <cstdint>
#include "driver/i2s_std.h"

struct MicrophoneConfig{
    i2s_port_t i2s_port;
    i2s_std_config_t i2s_configuration;
    i2s_std_gpio_config_t i2spinconfiguration;
    uint8_t channel_count;
    uint8_t SlotBitWidth_;

    bool validate(char* err);
    uint8_t GetSlotBitWidth(const i2s_std_config_t & cfg);

};