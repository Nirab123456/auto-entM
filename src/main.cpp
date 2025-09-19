//headers
#include <Arduino.h>
#include <WiFi.h>
#include <WiFiManager.h>
#include "driver/i2s.h"
#include "esp_timer.h"
#include <atomic>
#include <mutex>
#include <Preferences.h>
// configurations
const uint8_t RESET_WIFI_BUTTON_PIN = 4;
const uint16_t BUTTON_HOLD_MS = 800;
const char* WIFI_AP_NAME  = "auto-antm";
const char* WIFI_AP_PASS = "password";

//audio params
const uint8_t PIN_CLK = 7;
const uint8_t PIN_WS = 15;
const uint8_t PIN_SD =16;
const uint32_t SAMPLE_RATE = 48000;
const i2s_bits_per_sample_t I2S_BITS = I2S_BITS_PER_SAMPLE_32BIT;
const uint8_t NUMBERS_OF_CHANNELS = 1;
const uint16_t FRAMES_PER_PACKET = 1024;
const uint8_t BYTES_PER_SAMPLE =4;

const size_t BYTES_TO_READ = ((size_t)FRAMES_PER_PACKET*BYTES_PER_SAMPLE*2);
const size_t PAYLOAD_BYTES = ((size_t)FRAMES_PER_PACKET*BYTES_PER_SAMPLE*NUMBERS_OF_CHANNELS);
const size_t NEEDED_WORDS = ((size_t)FRAMES_PER_PACKET*2);

constexpr int HEADER_SIZE = 34;
const  uint32_t HEADER_MAGIC = 0x45535032;
const uint16_t FORMAT_INT32_LEFT24 =1;
const uint8_t HEADER_BUFFER[HEADER_SIZE];

//buffers
static uint32_t I2S_WORD_SLOTS[FRAMES_PER_PACKET*2];
static uint32_t payload_words[FRAMES_PER_PACKET];
//ring buffers
constexpr size_t RING_SIZE = 64;
constexpr size_t RING_MASK = RING_SIZE -1;
static uint32_t RUNG_PAYLOAD[RING_SIZE];
static uint64_t RING_TIMESTAMP[RING_SIZE];

//atomic variables
std::atomic<size_t> Ring_head{0};
std::atomic<size_t> Absolute_sample_index(0);
std::atomic<uint32_t> Sequence_counter{0};
std::atomic<uint64_t> Absolute_sample_index{0};
std::atomic<bool>consumer_ready{false};

//taskhandlers
TaskHandle_t audiohandleTASK = NULL;
TaskHandle_t networkhandleTASK = NULL;


WiFiClient TCPCLIENT;

static const char* PREF_NAMESPACE = "config";


class ReciverConfig{
private:
    Preferences prefs_;
    std :: mutex mu_;
    IPAddress ip_;
    uint16_t port_;
public:
    ReciverConfig(){}
    ~ReciverConfig(){
        prefs_.end();
    }

    void begin()
    {
        prefs_.begin(PREF_NAMESPACE,true);
        load();
        prefs_.end();
    }
    void load()
    {
        std::lock_guard<std::mutex> lock(mu_);
        prefs_.begin(PREF_NAMESPACE,true);
        String saved_ip = prefs_.getString("pc_ip","");
    }
};
