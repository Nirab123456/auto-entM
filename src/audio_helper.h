#pragma once
#include <Arduino.h>
#include "esp_timer.h"
#include <mutex>
#include <WiFiManager.h>
#include <WiFi.h>
#include "driver/i2s.h"
#include <atomic>
#include <Preferences.h>

// ---------- configurable values (inline or constexpr safe for header) ----------
inline const char* WIFI_AP_NAME  = "auto-antm";
inline const char* WIFI_AP_PASS  = "password";

// audio params
inline constexpr uint8_t PIN_CLK = 7;
inline constexpr uint8_t PIN_WS = 15;
inline constexpr uint8_t PIN_SD = 16;
inline constexpr uint32_t SAMPLE_RATE = 48000;
inline const i2s_bits_per_sample_t I2S_BITS = I2S_BITS_PER_SAMPLE_32BIT;
inline constexpr uint8_t DEFAULT_CHANNEL_COUNT = 2;
inline constexpr uint8_t BYTES_PER_SAMPLE = 4;
inline constexpr size_t NEEDED_WORDS = ((size_t)FRAMES_PER_PACKET * 2);

inline constexpr int HEADER_SIZE = 34;
inline constexpr uint32_t HEADER_MAGIC = 0x45535032;
inline constexpr uint16_t FORMAT_INT32_LEFT24 = 1;

// header buffer - extern (defined in .cpp)
extern uint8_t HEADER_BUFFER[HEADER_SIZE];

inline constexpr uint8_t MAIN_COPY_BYTES = 4;
inline constexpr uint8_t MIN_BITS_SHIFT = 8;

// ---------- buffers (declare here, define once in .cpp) ----------
extern uint32_t I2S_WORD_SLOTS[FRAMES_PER_PACKET * 2];
extern uint32_t payload_words[FRAMES_PER_PACKET];

// ring buffers (declare only)
inline constexpr size_t RING_SIZE = 64;
inline constexpr size_t RING_MASK = RING_SIZE - 1;
extern uint32_t RING_PAYLOAD[RING_SIZE][FRAMES_PER_PACKET];
extern uint64_t RING_TIMESTAMP[RING_SIZE];
extern uint16_t RING_FRAMES[RING_SIZE];
extern uint64_t RING_FIRST_INDEX[RING_SIZE];

// ---------- atomic/shared state (declare only) ----------
extern std::atomic<size_t> Ring_head;
extern std::atomic<size_t> Ring_tail;
extern std::atomic<uint32_t> Sequence_counter;
extern std::atomic<uint64_t> Absolute_sample_index;
extern std::atomic<bool> consumer_ready;

// ---------- task handles (declare only) ----------
extern TaskHandle_t audiohandleTASK;
extern TaskHandle_t networkhandleTASK;
extern TaskHandle_t printtaskHANDLE;
inline constexpr uint32_t PRINT_INTERVAL_MS = 2000;

inline const char* PREF_NAMESPACE = "config";

// Semaphores & ISR globals (declare only)
extern SemaphoreHandle_t button_semaphore;
extern volatile TickType_t isr_press_tick;
extern volatile TickType_t isr_last_edge_tick;
inline constexpr TickType_t debounce_ticks = pdMS_TO_TICKS(20);

// BUTTON PIN & TIMER
inline constexpr uint8_t RESET_WIFI_BUTTON_PIN = 4;
inline constexpr uint16_t BUTTON_HOLD_MS = 800;
inline constexpr TickType_t DELAYTICKS = pdMS_TO_TICKS(2000);

// TASK SIZE
inline constexpr uint16_t MONITOR_STACK = 4096;
inline constexpr uint16_t PRINT_STACK = 4096;

extern TaskHandle_t monitorhandleTASK;

// ---------- function prototypes ----------
void startconfigportal_button();
void setup_button_isr_TASK();
void startTASK();
void startmonitorTASK();
void printTASK(void* pv);
void printhandleTASK();
