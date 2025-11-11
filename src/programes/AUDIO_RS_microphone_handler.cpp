
#include "headers/microphoneConfig.h"
#include "headers/audio_read_send.h"
#include "headers/a_c_s.h"

void AUDIO_RS::set_micfg(const MicrophoneConfig &cfg)
{
    micfg_ = cfg;

    char errbuf[ERR_SZ];
    errbuf[0] = '\0';
    if (!micfg_.validate(errbuf))
    {
        Serial.printf("AUDIO_RS::set_micfg:: %s\n", errbuf);
        mic_configured_.store(false, std::memory_order_release);
    }
    else
    {
        mic_configured_.store(true, std::memory_order_release);
        if (CHANNEL_COUNT_)
        {
            CHANNEL_COUNT_->store(micfg_.channel_count, std::memory_order_relaxed);
        }
        
    }
}

bool MicrophoneConfig::validate(char* err)
{

    if (!err)
    {
        return false;
    }
    err[0] = '\0';

    if (channel_count !=1 && channel_count)
    {
        snprintf(err, ERR_SZ, "MicrophoneConfig::validate::Channel should be either Moni or Stereo::entered:%d", static_cast<int>(channel_count));
        return false;
    }

    if (!(
        i2s_port == I2S_NUM_0 ||
        i2s_port == I2S_NUM_1
    ))
    {
        snprintf(err, ERR_SZ, "MicrophoneConfig::Wrong I2S port (must be I2S_NUM_0 or I2S_NUM_1)");
        return false;    
    }

    if ((
        (i2s_configuration.mode & I2S_MODE_RX) == 0
    ))
    {
        snprintf(err, ERR_SZ, "MicrophoneConfig::i2s config must include I2S_MODE_RX");
        return false;    
    }
    
    // bits_per_sample allowed set
    if (!(i2s_configuration.bits_per_sample == I2S_BITS_PER_CHAN_16BIT ||
          i2s_configuration.bits_per_sample == I2S_BITS_PER_CHAN_24BIT ||
          i2s_configuration.bits_per_sample == I2S_BITS_PER_CHAN_32BIT)) {
        snprintf(err, ERR_SZ, "MicrophoneConfig::unsupported bits_per_sample value");
        return false;
    }

    // dma_buf_count sanity
    if (i2s_configuration.dma_buf_count < 1 || i2s_configuration.dma_buf_count > 12) {
        snprintf(err, ERR_SZ, "MicrophoneConfig::dma_buf_count out of range (1..12). Found=%d",
                 i2s_configuration.dma_buf_count);
        return false;
    }

    // dma_buf_len sanity
    if (i2s_configuration.dma_buf_len < 4 || i2s_configuration.dma_buf_len > 8192) {
        snprintf(err, ERR_SZ, "MicrophoneConfig::dma_buf_len out of range (4..8192). Found=%d",
                 i2s_configuration.dma_buf_len);
        return false;
    }

    // channel format must be one of the known constants
    {
        auto fmt = i2s_configuration.channel_format;
        bool ok =
            (fmt == I2S_CHANNEL_FMT_ONLY_RIGHT) ||
            (fmt == I2S_CHANNEL_FMT_ONLY_LEFT) ||
            (fmt == I2S_CHANNEL_FMT_RIGHT_LEFT) ||
            (fmt == I2S_CHANNEL_FMT_ALL_LEFT) ||
            (fmt == I2S_CHANNEL_FMT_ALL_RIGHT) ||
            (fmt == I2S_CHANNEL_FMT_MULTIPLE);
        if (!ok) {
            snprintf(err, ERR_SZ, "MicrophoneConfig::unsupported channel_format (value=%d)", static_cast<int>(fmt));
            return false;
        }
    }
    
    auto invalid_pin = [](int p)->bool {
        return (p == I2S_PIN_NO_CHANGE || p < 0 || p > 39);
    };
    
    if (invalid_pin(i2spinconfiguration.bck_io_num)) {
        snprintf(err, ERR_SZ, "MicrophoneConfig::bck_io_num invalid or I2S_PIN_NO_CHANGE");
        return false;
    }
    if (invalid_pin(i2spinconfiguration.ws_io_num)) {
        snprintf(err, ERR_SZ, "MicrophoneConfig::ws_io_num invalid or I2S_PIN_NO_CHANGE");
        return false;
    }
    
    if ((i2s_configuration.mode & I2S_MODE_RX) != 0)
    {
        if (invalid_pin(i2spinconfiguration.data_in_num)) {
            snprintf(err, ERR_SZ, "MicrophoneConfig::data_in_num invalid or I2S_PIN_NO_CHANGE (required for RX)");
            return false;
        }  
    }
    
    return true;
}