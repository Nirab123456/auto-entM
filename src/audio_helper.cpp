#include <Arduino.h>
#include "esp_timer.h"
#include <mutex>
#include <audio_helper.h>

//semaphore + TSR globals

static SemaphoreHandle_t button_semaphore = NULL;
static volatile TickType_t isr_press_tick = 0;
static volatile TickType_t isr_last_edge_tick =  0;
static const TickType_t debounce_ticks = pdMS_TO_TICKS(20);
const uint8_t RESET_WIFI_BUTTON_PIN = 4;
const uint16_t BUTTON_HOLD_MS = 800;



void IRAM_ATTR button_isr_handler()
{
  TickType_t now  = xTaskGetTickCountFromISR();
  if ((now-isr_last_edge_tick)<debounce_ticks)
  {
    isr_last_edge_tick = now;
    return;
  }
  isr_last_edge_tick = now;
  int state = digitalRead(RESET_WIFI_BUTTON_PIN);
  if (state == LOW)
  {
    isr_press_tick = now;
    return;
  }
  else{
    TickType_t press_tick = isr_press_tick;
    if (press_tick == 0)
    {
      return;
    }
    TickType_t dur_ticks = now - press_tick;
    uint32_t dur_ms = ((uint32_t)(dur_ms*portTICK_PERIOD_MS));
    if (dur_ms >= BUTTON_HOLD_MS)
    {
      BaseType_t xHigh_priority_task_open = pdFALSE;
      xSemaphoreGiveFromISR(button_semaphore, &xHigh_priority_task_open);
      if (xHigh_priority_task_open == pdTRUE)
      {
        portYIELD_FROM_ISR();
      } 
    }
    isr_press_tick = 0;
  }
}
 void button_handle_TASK(void*pv)
 {
  (void)pv;
  for (;;)
  {
    if (xSemaphoreTake(button_semaphore,portMAX_DELAY)== pdTRUE)
    {
      Serial.println("BUTTON : LOng press detected --> start config portal");
      startconfigportal_button();
      vTaskDelay(pdMS_TO_TICKS(500));
    }
  }
  vTaskDelete(NULL);
 }


 void setup_button_isr_TASK()
 {
  button_semaphore = xSemaphoreCreateBinary();
  attachInterrupt(digitalPinToInterrupt(RESET_WIFI_BUTTON_PIN),button_isr_handler,CHANGE);
  BaseType_t ok = xTaskCreate(button_handle_TASK,"button_handler_task",3072,NULL,2,NULL);
  if (ok!= pdPASS)
  {
    Serial.println("BUTTON: Task create failed");
  }
  else
  {
    Serial.println("BUTTON: button handler task :: created");
  }
  
 }