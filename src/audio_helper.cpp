#include <Arduino.h>
#include "esp_timer.h"
#include <mutex>
#include <Preferences.h>
#include "audio_helper.h"


// semaphore + ISR globals
static SemaphoreHandle_t button_semaphore = NULL;
static volatile TickType_t isr_press_tick = 0;
static volatile TickType_t isr_last_edge_tick = 0;
static const TickType_t debounce_ticks = pdMS_TO_TICKS(20);

// pins / timing
const uint8_t RESET_WIFI_BUTTON_PIN = 4;
const uint16_t BUTTON_HOLD_MS = 800;
const TickType_t DELAYTICKS = pdMS_TO_TICKS(2000);

// stack sizes for created tasks
const uint16_t MONITOR_STACK = 4096;
const uint16_t PRINT_STACK = 4096;

// monitor task handle local to this module (keeps ownership)
static TaskHandle_t monitorhandleTASK = NULL;

// =======================================================
// ISR: button press detection (short/long press). Keep ISR short.
// =======================================================
void IRAM_ATTR button_isr_handler()
{
  TickType_t now = xTaskGetTickCountFromISR();

  // debounce
  if ((now - isr_last_edge_tick) < debounce_ticks) {
    isr_last_edge_tick = now;
    return;
  }
  isr_last_edge_tick = now;

  int state = digitalRead(RESET_WIFI_BUTTON_PIN);
  if (state == LOW) {
    // button pressed
    isr_press_tick = now;
    return;
  } else {
    // button released
    TickType_t press_tick = isr_press_tick;
    if (press_tick == 0) return;

    TickType_t dur_ticks = now - press_tick;
    uint32_t dur_ms = (uint32_t)pdTICKS_TO_MS(dur_ticks);

    if (dur_ms >= BUTTON_HOLD_MS) {
      BaseType_t xHigherPriorityTaskWoken = pdFALSE;
      if (button_semaphore) {
        xSemaphoreGiveFromISR(button_semaphore, &xHigherPriorityTaskWoken);
        if (xHigherPriorityTaskWoken == pdTRUE) {
          portYIELD_FROM_ISR();
        }
      }
    }
    isr_press_tick = 0;
  }
}

// =======================================================
// button handler task: wait for semaphore, then perform action
// =======================================================
void button_handle_TASK(void* pv)
{
  (void)pv;
  for (;;) {
    if (button_semaphore == NULL) {
      // Should not happen but guard defensively
      vTaskDelay(pdMS_TO_TICKS(100));
      continue;
    }
    if (xSemaphoreTake(button_semaphore, portMAX_DELAY) == pdTRUE) {
      Serial.println("BUTTON : Long press detected -> start config portal");
      // Call out to main module function that starts config portal
      startconfigportal_button();
      vTaskDelay(pdMS_TO_TICKS(500));
    }
  }
  vTaskDelete(NULL);
}

// =======================================================
// Install ISR, create semaphore + button handler task
// =======================================================
void setup_button_isr_TASK()
{
  // create the binary semaphore first
  if (button_semaphore == NULL) {
    button_semaphore = xSemaphoreCreateBinary();
    if (button_semaphore == NULL) {
      Serial.println("BUTTON: Failed to create semaphore");
      return;
    }
  }

  // configure pin and attach interrupt (use INPUT_PULLUP for a button to GND)
  pinMode(RESET_WIFI_BUTTON_PIN, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(RESET_WIFI_BUTTON_PIN), button_isr_handler, CHANGE);

  // create the handler task (low priority)
  BaseType_t ok = xTaskCreate(button_handle_TASK, "button_handler_task", 3072, NULL, 3, NULL);
  if (ok != pdPASS) {
    Serial.println("BUTTON: Task create failed");
  } else {
    Serial.println("BUTTON: button handler task created");
  }
}

// =======================================================
// monitor task: simple watchdog / restart tasks if missing
// =======================================================
void monitorTASK(void* pv)
{
  (void)pv;
  for (;;) {
    vTaskDelay(DELAYTICKS);

    // ensure main tasks exist; if not, attempt to (re)start them
    if (audiohandleTASK == NULL || networkhandleTASK == NULL) {
      Serial.println("MONITOR: Detected missing tasks, attempting startTASK()");
      startTASK();
    }

    if (audiohandleTASK != NULL) {
      UBaseType_t hw = uxTaskGetStackHighWaterMark(audiohandleTASK);
      Serial.printf("MONITOR TASK :: AUDIO TASK :: High watermark %u\n", (unsigned)hw);
    }
  }
  vTaskDelete(NULL);
}

// Create monitor task (pin to core 0)
void startmonitorTASK()
{
  if (monitorhandleTASK == NULL) {
    BaseType_t ok = xTaskCreatePinnedToCore(
      monitorTASK,
      "monitorTASK",
      MONITOR_STACK,
      NULL,
      2,
      &monitorhandleTASK,
      0
    );
    if (ok != pdPASS) {
      Serial.println("MONITOR TASK: Creation Failed");
      monitorhandleTASK = NULL;
    } else {
      Serial.println("MONITOR TASK: Created");
    }
  } else {
    Serial.println("MONITOR TASK: Already running");
  }
}

// =======================================================
// print-task creator: spawn the existing printTASK from main
// Note: printTASK(void*) must be defined in the main module.
// =======================================================
void printhandleTASK()
{
  if (printtaskHANDLE == NULL) {
    BaseType_t ok = xTaskCreatePinnedToCore(printTASK, "printTASK", PRINT_STACK, NULL, 1, &printtaskHANDLE, 1);
    if (ok != pdPASS) {
      Serial.println("PRINT TASK: Failure creating");
      printtaskHANDLE = NULL;
    } else {
      Serial.println("PRINT TASK: Created");
    }
  } else {
    Serial.println("PRINT TASK: Already running");
  }
}
