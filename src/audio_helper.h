#pragma once
#include <Arduino.h>
#include "esp_timer.h"
#include <mutex>
#include <WiFiManager.h>

void startconfigportal_button();
void setup_button_isr_TASK();
void startTASK();
void startmonitorTASK();
void printTASK(void*pv);
void printhandleTASK();

extern TaskHandle_t audiohandleTASK;
extern TaskHandle_t networkhandleTASK;
extern TaskHandle_t printtaskHANDLE;

