// ======= place near top of file with globals =======
#include <Arduino.h>
#include "esp_timer.h"
#include <mutex>
#include <button_p.h>
// semaphore + ISR timestamp globals
static SemaphoreHandle_t buttonSem = NULL;            // given from ISR to handler task
static volatile TickType_t isr_press_tick = 0;        // tick at falling-edge (press start)
static volatile TickType_t isr_last_edge_tick = 0;    // for debounce in ISR
static const TickType_t debounce_ticks = pdMS_TO_TICKS(20); // 20 ms debounce
const uint8_t RESET_WIFI_BUTTON_PIN = 4;
const uint16_t BUTTON_HOLD_MS = 800;
// ======= ISR (very small, safe) =======
// ISR must be minimal and placed in IRAM
void IRAM_ATTR button_isr_handler() {
  TickType_t now = xTaskGetTickCountFromISR();

  // very small debounce in ISR
  if ((now - isr_last_edge_tick) < debounce_ticks) {
    isr_last_edge_tick = now;
    return;
  }
  isr_last_edge_tick = now;

  // Read pin quickly (digitalRead is ok in ESP32 ISR)
  int state = digitalRead(RESET_WIFI_BUTTON_PIN);

  if (state == LOW) {
    // falling edge -> record press start tick
    isr_press_tick = now;
    return;
  } else {
    // rising edge -> calculate duration
    TickType_t press_tick = isr_press_tick;
    if (press_tick == 0) return;
    TickType_t dur_ticks = now - press_tick;
    uint32_t dur_ms = (uint32_t)(dur_ticks * portTICK_PERIOD_MS);
    // if held long enough, signal handler task
    if (dur_ms >= BUTTON_HOLD_MS) {
      BaseType_t xHigherPriorityTaskWoken = pdFALSE;
      xSemaphoreGiveFromISR(buttonSem, &xHigherPriorityTaskWoken);
      if (xHigherPriorityTaskWoken == pdTRUE) portYIELD_FROM_ISR();
    }
    // reset
    isr_press_tick = 0;
  }
}



// ======= handler task: waits for the semaphore and calls reset flow =======
void buttonHandlerTask(void *pv) {
  (void)pv;
  for (;;) {
    // Wait indefinitely for the semaphore from the ISR
    if (xSemaphoreTake(buttonSem, portMAX_DELAY) == pdTRUE) {
      // We are in a normal task context — safe to call heavy functions
      Serial.println("[BUTTON] long-press detected (handler task) -> start config portal");
      // CALL your heavy reset flow here
      startconfigportal_button();
      // small delay to avoid immediate re-trigger
      vTaskDelay(pdMS_TO_TICKS(500));
    }
  }
  vTaskDelete(NULL);
}

// ======= call this from setup() to initialize ISR + task =======
void setup_button_isr_and_task() {
  // create binary semaphore
  buttonSem = xSemaphoreCreateBinary();
  if (buttonSem == NULL) {
    Serial.println("[BUTTON] failed to create semaphore!");
    return;
  }

  // configure pin and attach ISR on CHANGE so we see press AND release
  pinMode(RESET_WIFI_BUTTON_PIN, INPUT_PULLUP); // keep pin mode setup here so setup() doesn't need to
  attachInterrupt(digitalPinToInterrupt(RESET_WIFI_BUTTON_PIN), button_isr_handler, CHANGE);

  // create handler task with low priority (so it won't preempt audio task)
  BaseType_t ok = xTaskCreate(buttonHandlerTask, "btnHandler", 3072, NULL, 2, NULL);
  if (ok != pdPASS) Serial.println("[BUTTON] handler task create failed");
  else Serial.println("[BUTTON] ISR + handler task installed");
}
