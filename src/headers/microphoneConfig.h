#pragma once 
#include <Arduino.h>
#include <span>
#include <atomic>
#include <memory>
#include <cstdint>
#include "driver/i2s.h"
#include <string>

struct MicrophoneConfig {

    // i2s port and pins (user supplies)
    i2s_port_t i2s_port;
    i2s_config_t i2s_configuration;
    i2s_pin_config_t i2spinconfiguration;
    bool validate(char* err) const {
        // 1) i2s_port sanity
        if (!(i2s_port == I2S_NUM_0 || i2s_port == I2S_NUM_1)) {
            err = "Invalid i2s_port (must be I2S_NUM_0 or I2S_NUM_1).";
            return false;
        }

        // 2) mode: ensure RX (capture) bit present for microphone usage
        if ((i2s_configuration.mode & I2S_MODE_RX) == 0) {
            err = "I2S config must include I2S_MODE_RX for microphone capture.";
            return false;
        }

        // 3) sample rate range (practical limits)
        if (i2s_configuration.sample_rate < 8000 || i2s_configuration.sample_rate > 192000) {
            err = "sample_rate out of practical range (8k..192k).";
            return false;
        }

        // 4) bits per sample support
        if (!(i2s_configuration.bits_per_sample == I2S_BITS_PER_SAMPLE_16BIT ||
              i2s_configuration.bits_per_sample == I2S_BITS_PER_SAMPLE_24BIT ||
              i2s_configuration.bits_per_sample == I2S_BITS_PER_SAMPLE_32BIT)) {
            err = "Unsupported bits_per_sample. Use 16, 24 or 32 bit constants.";
            return false;
        }

        // 5) channel format check
        // allow right-left stereo or single-channel formats
        if (!(i2s_configuration.channel_format == I2S_CHANNEL_FMT_RIGHT_LEFT ||
              i2s_configuration.channel_format == I2S_CHANNEL_FMT_ONLY_RIGHT ||
              i2s_configuration.channel_format == I2S_CHANNEL_FMT_ONLY_LEFT)) {
            err = "Unsupported channel_format. Use RIGHT_LEFT or ONLY_LEFT/ONLY_RIGHT.";
            return false;
        }

        // 6) DMA buffer sizing sanity (simple bounds)
        if (i2s_configuration.dma_buf_count < 1 || i2s_configuration.dma_buf_count > 12) {
            err = "dma_buf_count out of range (1..12).";
            return false;
        }
        if (i2s_configuration.dma_buf_len < 4 || i2s_configuration.dma_buf_len > 8192) {
            err = "dma_buf_len out of range (4..8192).";
            return false;
        }

        // 7) pins: check they are set and not equal to I2S_PIN_NO_CHANGE (-1)
        // Note: boards vary which GPIOs are valid. This just performs a simple numeric check.
        auto invalid_pin = [](int p)->bool { return p == I2S_PIN_NO_CHANGE || p < 0 || p > 39; };
        if (invalid_pin(i2spinconfiguration.bck_io_num) ||
            invalid_pin(i2spinconfiguration.ws_io_num)  ||
            (i2s_configuration.mode & I2S_MODE_RX ? invalid_pin(i2spinconfiguration.data_in_num) : false)) {
            err = "I2S pin config: pins must be set and within 0..39 (or board-specific).";
            return false;
        }

        // // 8) optional board-specific extra checks
        // if (extra_validator) {
        //     std::string extra_err;
        //     if (!extra_validator(*this, extra_err)) {
        //         err = std::string("extra_validator failed: ") + extra_err;
        //         return false;
        //     }
        // }

        // All light checks passed
        return true;
    }
};

