// main.cpp — use ReciverConfig to provide receiver IP dynamically
// Build with -std=gnu++20 (std::span)

#include <Arduino.h>
#include <WiFi.h>
#include <memory>
#include <atomic>
#include <span>

#include "headers/audio_read_send.h"   // AUDIO_RS
#include "headers/microphoneConfig.h"  // MicrophoneConfig
#include "headers/ReciverConfig.h"     // ReciverConfig (uses Preferences internally)

// ---------- Constants (tweak as needed) ----------
constexpr size_t FRAMES_PER_PACKET = 1024;
constexpr size_t RING_SIZE = 64; // power of two recommended
static_assert((RING_SIZE & (RING_SIZE - 1)) == 0, "RING_SIZE should be power of two");
constexpr size_t I2S_WORD_SLOTS_LEN = FRAMES_PER_PACKET * 2;
constexpr size_t RING_FLAT_LEN = RING_SIZE * FRAMES_PER_PACKET;

// ---------- Global buffers (static, contiguous) ----------
static uint32_t I2S_WORD_SLOTS[I2S_WORD_SLOTS_LEN];
static uint32_t RING_PAYLOAD[RING_SIZE][FRAMES_PER_PACKET];

// metadata arrays (static lifetime)
static uint16_t RING_FRAMES[RING_SIZE];
static uint64_t RING_FIRST_INDEX[RING_SIZE];
static uint64_t RING_TIMESTAMP[RING_SIZE];

// helper: flat span from 2D array
inline std::span<uint32_t> make_ring_flat_span() {
    return std::span<uint32_t>(&RING_PAYLOAD[0][0], RING_FLAT_LEN);
}

// ---------- shared atomics ----------
static auto g_consumer_ready   = std::make_shared<std::atomic<bool>>(false);
static auto g_ring_head        = std::make_shared<std::atomic<size_t>>(0);
static auto g_ring_tail        = std::make_shared<std::atomic<size_t>>(0);
static auto g_abs_idx          = std::make_shared<std::atomic<uint64_t>>(0);
static auto g_sequence_counter = std::make_shared<std::atomic<uint32_t>>(0);

// ---------- AUDIO_RS instance (static to guarantee lifetime) ----------
static AUDIO_RS audio_instance;

// ---------- ReciverConfig instance ----------
static ReciverConfig recfg;

// ---------- WiFi credentials (use your network, or let station auto-connect) ----------
const char* WIFI_SSID = "YOUR_SSID";
const char* WIFI_PASS = "YOUR_PASSWORD";

// ---------- helper spans for metadata ----------
static std::span<uint16_t> ring_frames_span(RING_FRAMES, RING_SIZE);
static std::span<uint64_t> ring_first_index_span(RING_FIRST_INDEX, RING_SIZE);
static std::span<uint64_t> ring_timestamp_span(RING_TIMESTAMP, RING_SIZE);

void setup() {
    // minimal serial for debugging only (optional). Remove if you truly want no serial.
    Serial.begin(115200);
    delay(200);

    // 1) Prepare spans (no copy)
    std::span<uint32_t> i2s_span(I2S_WORD_SLOTS, I2S_WORD_SLOTS_LEN);
    std::span<uint32_t> ring_span = make_ring_flat_span();

    // 2) Provide buffers & atomics to audio_instance
    audio_instance.set_i2s_buffer(i2s_span);
    audio_instance.set_ring_payload_flat(ring_span, FRAMES_PER_PACKET);

    audio_instance.set_consumer_ready(g_consumer_ready);
    audio_instance.set_ring_head(g_ring_head);
    audio_instance.set_ring_tail(g_ring_tail);
    audio_instance.set_abs_idx(g_abs_idx);
    audio_instance.set_sequence_counter(g_sequence_counter);

    audio_instance.set_ring_metadata_spans(ring_frames_span, ring_first_index_span, ring_timestamp_span);

    // 3) Microphone config — fill appropriately for your board
    MicrophoneConfig miccfg;
    // TODO: populate miccfg fields (i2s_port, i2s_configuration, i2spinconfiguration)
    audio_instance.set_micfg(miccfg);

    // 4) Initialize ReciverConfig (loads saved receiver IP/port from Preferences)
    recfg.begin(); // loads saved ip/port into recfg internally

    // Register ReciverConfig pointer with audio_instance so network tasks can call ConnectTOReciverIP(...)
    audio_instance.set_reciver_config_ptr(&recfg);

    // 5) WiFi connect (station). We don't prompt serial; just attempt to connect.
    WiFi.begin(WIFI_SSID, WIFI_PASS);
    unsigned long started = millis();
    const unsigned long timeout_ms = 10000;
    while (WiFi.status() != WL_CONNECTED && (millis() - started) < timeout_ms) {
        vTaskDelay(pdMS_TO_TICKS(200));
    }
    if (WiFi.status() == WL_CONNECTED) {
        Serial.println("WiFi connected");
    } else {
        Serial.println("WiFi not connected (continuing — network tasks will retry)");
    }

    // 6) init I2S
    if (!audio_instance.initI2S()) {
        Serial.println("I2S init failed");
        // optionally retry or abort — here we continue so tasks will early-exit
    }

    // 7) Configure network writer to use ReciverConfig internally (your AUDIO_RS Network loop uses recfg_ptr_)
    // If you want to provide a WiFiClient pointer, pass it too:
    static WiFiClient tcp_client;
    audio_instance.set_tcp_client_ptr(&tcp_client);

    // 8) Start tasks (trampolines already declared in header)
    TaskHandle_t h;
    audio_instance.start_task("I2SRead", 4096, 2, 1, AUDIO_RS::I2SReadTrampoline, &audio_instance, &h);
    audio_instance.start_task("RingWriter", 8192, 2, 1, AUDIO_RS::RingWriterFRMI2STrampoline, &audio_instance, &h);
    audio_instance.start_task("NetworkTask", 8192, 2, 1, AUDIO_RS::NetworkTaskLoopTrampoline, &audio_instance, &h);
    audio_instance.start_task("NetWriter", 8192, 2, 1, AUDIO_RS::NetworkDataWriterLoopTrampoline, &audio_instance, &h);

    // no console input — ReciverConfig determines where we connect (from stored preferences)
    // Optionally you can programmatically update recfg.save("192.168.x.y", port) elsewhere (OTA/UI).
}

void loop() {
    // Idle; tasks do the work.
    vTaskDelay(pdMS_TO_TICKS(5000));
}
