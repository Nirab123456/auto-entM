
#include "headers/audio_read_send.h"   // your AUDIO_RS header (adjust path if needed)
#include "headers/microphoneConfig.h" // your MicrophoneConfig definition (adjust path)
#include "headers/ReciverConfig.h"    // optional, if used for network connect

// ---------- Constants (tweak as needed) ----------
constexpr size_t FRAMES_PER_PACKET = 1024;        // match what your class expects
constexpr size_t RING_SIZE = 64;                  // number of ring slots (power of two recommended)
static_assert((RING_SIZE & (RING_SIZE - 1)) == 0, "RING_SIZE should be power of two for mask optimization");
constexpr size_t I2S_WORD_SLOTS_LEN = FRAMES_PER_PACKET * 2; // stereo words buffer (example)
constexpr size_t RING_FLAT_LEN = RING_SIZE * FRAMES_PER_PACKET;

// ---------- Global RAM buffers (static, contiguous) ----------
static uint32_t I2S_WORD_SLOTS[I2S_WORD_SLOTS_LEN];                 // DMA read target
static uint32_t RING_PAYLOAD[RING_SIZE][FRAMES_PER_PACKET];         // contiguous 2D array

// metadata arrays (must live as long as AUDIO_RS uses them)
static uint16_t RING_FRAMES[RING_SIZE];
static uint64_t RING_FIRST_INDEX[RING_SIZE];
static uint64_t RING_TIMESTAMP[RING_SIZE];

// ---------- Helper to make flat span from 2D array ----------
inline std::span<uint32_t> make_ring_flat_span()
{
    // pointer to first element of 2D C-array is &RING_PAYLOAD[0][0]
    return std::span<uint32_t>(&RING_PAYLOAD[0][0], RING_FLAT_LEN);
}

// ---------- Shared atomics (shared ownership) ----------
auto make_shared_atomic_bool(bool init = false) {
    return std::make_shared<std::atomic<bool>>(init);
}
auto make_shared_atomic_size_t(size_t init = 0) {
    return std::make_shared<std::atomic<size_t>>(init);
}
auto make_shared_atomic_uint64(uint64_t init = 0) {
    return std::make_shared<std::atomic<uint64_t>>(init);
}
auto make_shared_atomic_uint32(uint32_t init = 0) {
    return std::make_shared<std::atomic<uint32_t>>(init);
}

// ---------- Global shared_ptrs you can inspect in the debugger ----------
std::shared_ptr<std::atomic<bool>>  g_consumer_ready;
std::shared_ptr<std::atomic<size_t>> g_ring_head;
std::shared_ptr<std::atomic<size_t>> g_ring_tail;
std::shared_ptr<std::atomic<uint64_t>> g_abs_idx;
std::shared_ptr<std::atomic<uint32_t>> g_sequence_counter;

// ---------- AUDIO_RS instance (static to guarantee lifetime) ----------
static AUDIO_RS audio_instance{};

// ---------- Simple Reciver connectivity (adjust for your network) ----------
const char* WIFI_SSID = "YOUR_SSID";
const char* WIFI_PASS = "YOUR_WIFI_PASSWORD";
// receiver IP/port — replace with actual receiver
IPAddress receiver_ip(192,168,1,100);
uint16_t receiver_port = 4000;

// ---------- optional WiFiClient if you want to use one ----------------
static WiFiClient tcp_client;

// ---------- helper: prepare metadata spans for AUDIO_RS ----------
std::span<uint16_t> ring_frames_span(RING_FRAMES, RING_SIZE);
std::span<uint64_t> ring_first_index_span(RING_FIRST_INDEX, RING_SIZE);
std::span<uint64_t> ring_timestamp_span(RING_TIMESTAMP, RING_SIZE);

// ---------- Setup: create atomics, spans, configure mic, start tasks ----
void setup() {
    Serial.begin(115200);
    delay(500);
    Serial.println("Starting AUDIO_RS example wiring...");

    // 1) Initialize global atomics
    g_consumer_ready = make_shared_atomic_bool(false);
    g_ring_head = make_shared_atomic_size_t(0);
    g_ring_tail = make_shared_atomic_size_t(0);
    g_abs_idx = make_shared_atomic_uint64(0);
    g_sequence_counter = make_shared_atomic_uint32(0);

    // 2) Prepare spans (no copy, non-owning)
    std::span<uint32_t> i2s_span(I2S_WORD_SLOTS, I2S_WORD_SLOTS_LEN);
    std::span<uint32_t> ring_span = make_ring_flat_span();

    // 3) Construct/initialize the AUDIO_RS instance
    // NOTE: using default constructor above, now set buffers & atomics
    audio_instance.set_i2s_buffer(i2s_span);
    audio_instance.set_ring_payload_flat(ring_span, FRAMES_PER_PACKET);

    audio_instance.set_consumer_ready(g_consumer_ready);
    audio_instance.set_ring_head(g_ring_head);
    audio_instance.set_ring_tail(g_ring_tail);
    audio_instance.set_abs_idx(g_abs_idx);
    audio_instance.set_sequence_counter(g_sequence_counter);

    // 4) Provide metadata spans (so the class can write frame counts / timestamps)
    audio_instance.set_ring_metadata_spans(ring_frames_span, ring_first_index_span, ring_timestamp_span);

    // 5) Configure microphone (you must fill MicrophoneConfig with correct pins/params)
    MicrophoneConfig miccfg;
    // TODO: set miccfg fields to match your microphone / board:
    // miccfg.i2s_port = I2S_NUM_0; miccfg.i2s_configuration = {...} ; miccfg.i2spinconfiguration = {...}
    // Example: set port and default sample format, channel, etc. Refer to your board config API.
    audio_instance.set_micfg(miccfg);

    // 6) init I2S driver
    if (!audio_instance.initI2S()) {
        Serial.println("Failed to init I2S - check mic config");
        // handle failure; in this example we abort
        return;
    }

    // 7) (Optional) network setup — connect to WiFi
    Serial.printf("Connecting to WiFi SSID='%s'\n", WIFI_SSID);
    WiFi.begin(WIFI_SSID, WIFI_PASS);
    unsigned long wifi_start = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - wifi_start < 10000) {
        vTaskDelay(pdMS_TO_TICKS(200));
        Serial.print(".");
    }
    Serial.println();
    if (WiFi.status() == WL_CONNECTED) {
        Serial.printf("WiFi connected, IP=%s\n", WiFi.localIP().toString().c_str());
    } else {
        Serial.println("WiFi not connected (continuing in best-effort mode)");
    }

    // 8) Configure TCP client pointer (optional). If you use ReciverConfig, set that instead.
    audio_instance.set_tcp_client_ptr(&tcp_client);

    // 9) header buffer size & header writer callback (optional)
    audio_instance.set_header_buffer_size(64); // reserve header bytes (example)
    audio_instance.set_write_tcp_header_fn([](uint32_t seq, uint64_t first_index, uint64_t ts, uint16_t frames) {
        // If you want custom header generation, do it here (for network writer to call)
        // For demo we just print
        Serial.printf("WriteHeader: seq=%u idx=%llu ts=%llu frames=%u\n", seq, (unsigned long long)first_index, (unsigned long long)ts, frames);
    });

    // 10) Start internal tasks — use trampolines declared in class header
    // I2S read task — reads I2S and pushes read_bytes into i2s_queue_
    audio_instance.start_task("I2SRead", 4096, 2, 1, AUDIO_RS::I2SReadTrampoline, &audio_instance, &audio_instance.i2s_reader_handle_);

    // Ring writer (reads i2s_queue_ and writes into ring slots)
    audio_instance.start_task("RingWriter", 8192, 2, 1, AUDIO_RS::RingWriterFRMI2STrampoline, &audio_instance, &audio_instance.ring_writer_handle_);

    // Network task: monitors ring tail/head and queues up slots for writer
    audio_instance.start_task("NetworkTask", 8192, 2, 1, AUDIO_RS::NetworkTaskLoopTrampoline, &audio_instance, &audio_instance.network_handle_);

    // Network writer: sends payload for slots pulled from network_slot_queue_
    audio_instance.start_task("NetWriter", 8192, 2, 1, AUDIO_RS::NetworkDataWriterLoopTrampoline, &audio_instance, &audio_instance.network_writer_handle_);

    Serial.println("All tasks started.");
}

void loop() {
    // Nothing here — tasks do the work
    vTaskDelay(pdMS_TO_TICKS(5000));
}
