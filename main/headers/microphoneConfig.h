#pragma once 
#include <Arduino.h>
#include <span>
#include <atomic>
#include <memory>
#include <cstdint>
#include "driver/i2s.h"

struct MicrophoneConfig{
    i2s_port_t i2s_port;
    i2s_config_t i2s_configuration;
    i2s_pin_config_t i2spinconfiguration;
    uint8_t channel_count;

    bool validate(char* err);

};