#pragma once
#include <Arduino.h>

class AUDIO_RS
{
private:
    /* data */
    void RING_Clear_Rst();
public:
    AUDIO_RS(/* args */);
    void Audio_TASK(void* pv);

};


