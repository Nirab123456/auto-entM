
#include "headers/microphoneConfig.h"
#include "headers/audio_read_send.h"
#include "headers/a_c_s.h"

static const char *mhTAG = "AUDIO_RS_microphone_handler";

void AUDIO_RS::set_micfg(const MicrophoneConfig &cfg)
{
    micfg_ = cfg;

    char errbuf[ERR_SZ];
    errbuf[0] = '\0';
    if (!micfg_.validate(errbuf))
    {
        ESP_LOGE(mhTAG, "AUDIO_RS::set_micfg:: %s\n", errbuf);
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
    
    uint8_t sbw = 0;

    sbw = GetSlotBitWidth(i2s_configuration);
    if (sbw > 0)
    {   
        SlotBitWidth_ = sbw;
    }
    else
    {
        return false;
    }
    

    return true;
}

uint8_t MicrophoneConfig::GetSlotBitWidth(const i2s_std_config_t &cfg)
{
    switch (cfg.slot_cfg.data_bit_width)
    {
    case I2S_DATA_BIT_WIDTH_8BIT:
        return 8;
    case I2S_DATA_BIT_WIDTH_16BIT:
        return 16;
    case I2S_DATA_BIT_WIDTH_24BIT:
        return 24;
    case I2S_DATA_BIT_WIDTH_32BIT:
        return 32;
    default:
        return 0;
    }
}