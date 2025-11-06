
#include "headers/microphoneConfig.h"
#include "headers/audio_read_send.h"

void AUDIO_RS::set_micfg(const MicrophoneConfig &cfg)
{
    micfg_ = cfg;
    char* s = nullptr;
    bool ok = micfg_.validate(s);
    mic_configured_.store(ok, std::memory_order_release);
    if (CHANNEL_COUNT_)
    {
        CHANNEL_COUNT_->store(micfg_.channel_count,std::memory_order_relaxed);
    }   
}
bool MicrophoneConfig::validate(char*& err)
{
    if (channel_count != 1 || channel_count !=2)
    {
        err = "MicrophoneConfig::Channel should be either MONO / STEREO";
        return false;
    }
    
    if (!(i2s_port == I2S_NUM_0 || i2s_port == I2S_NUM_1))
    {
        err = "MicrophoneConfig::Wrong I2s port";
        return false;
    }

    if ((i2s_configuration.mode & I2S_MODE_RX) == 0)
    {
        err = "MicrophoneConfig::i2s config must include I2s_MODE_RX";
        return false;
    }
    
    if (!(
        i2s_configuration.bits_per_sample == I2S_BITS_PER_CHAN_16BIT ||
        i2s_configuration.bits_per_sample == I2S_BITS_PER_CHAN_24BIT ||
        i2s_configuration.bits_per_sample == I2S_BITS_PER_CHAN_32BIT
    ))
    {
        err = "MicrophoneConfig::unsupported bits_per_sample";
        return false;
    }
    if (i2s_configuration.dma_buf_count < 1 || i2s_configuration.dma_buf_count > 12)
    {
        err = "MicrophoneConfig:: dma_buf_count out of range";
        return false;
    }
    if (i2s_configuration.dma_buf_len < 4 || i2s_configuration.dma_buf_len > 8192) {
        err = "dma_buf_len out of range (4..8192).";
        return false;
    }
    if (
        i2s_configuration.channel_format != I2S_CHANNEL_FMT_ONLY_RIGHT ||
        i2s_configuration.channel_format != I2S_CHANNEL_FMT_ONLY_LEFT  ||
        i2s_configuration.channel_format != I2S_CHANNEL_FMT_RIGHT_LEFT ||
        i2s_configuration.channel_format != I2S_CHANNEL_FMT_MULTIPLE ||
        i2s_configuration.channel_format != I2S_CHANNEL_FMT_ALL_LEFT ||
        i2s_configuration.channel_format != I2S_CHANNEL_FMT_ALL_RIGHT
    )
    {
        return false;        
    }
    

    auto invalid_pin = [](int p)->bool
    {
        return (p == I2S_PIN_NO_CHANGE || p < 0 || p > 39);
    };
    if (
        invalid_pin(i2spinconfiguration.bck_io_num) || 
        invalid_pin(i2spinconfiguration.ws_io_num) ||
        i2s_configuration.mode & I2S_MODE_RX ? invalid_pin(i2spinconfiguration.data_in_num) : false
    )
    {
        err = "MicrophoneConfig::i2s pins must be within bound (0 - 39) or board spesific";
        return false;   
    }
    return true;   
}