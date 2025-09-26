#pragma once
#include <Arduino.h>
#include "esp_timer.h"
#include <mutex>
#include "button_p.h"

void startconfigportal_button();
void setup_button_isr_and_task();
