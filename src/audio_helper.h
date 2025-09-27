#pragma once
#include <Arduino.h>
#include "esp_timer.h"
#include <mutex>

void startconfigportal_button();
void setup_button_isr_TASK();
void startTASK();
void startmonitorTASK();

extern TaskHandle_t audiohandleTASK;
extern TaskHandle_t networkhandleTASK;