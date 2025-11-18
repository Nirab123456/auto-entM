#include "headers/psramalloc.h"
#include "main_helper.h"
#include <stdlib.h>


static const char *mainTAG = "main_AUDIO_RS";

static uint32_t I2S_WORD_SLOTS[I2S_WORD_SLOTS_LEN];
static uint32_t* RING_PAYLOAD_FLAT = nullptr;
static uint16_t RING_FRAMES[RING_SIZE];
static uint64_t RING_TIMESTAMP[RING_SIZE];
static uint64_t RING_FIRST_INDEX[RING_SIZE];

inline static std::span<uint16_t> ring_frames_span(RING_FRAMES, RING_SIZE);
inline static std::span<uint64_t> ring_first_index_span(RING_FIRST_INDEX, RING_SIZE);
inline static std::span<uint64_t> ring_ts_us_span(RING_TIMESTAMP, RING_SIZE);




inline std::span<uint32_t> make_ring_flat_span()
{
    return std::span<uint32_t>(RING_PAYLOAD_FLAT, RING_FLAT_LEN);
}


//shared ptrs 
std::shared_ptr<std::atomic<bool>> g_consumer_ready;
std::shared_ptr<std::atomic<size_t>> g_ring_head;
std::shared_ptr<std::atomic<size_t>> g_ring_tail;
std::shared_ptr<std::atomic<uint64_t>> g_abs_idx;
std::shared_ptr<std::atomic<uint32_t>> g_sequence_counter;

static ReciverConfig recivercfg("config");

static AUDIO_RS audio_rs_instance;

static WiFiClient WiFi_client;

MicrophoneConfig miccfg;

static void startup_task(void* pv)
{
    ESP_LOGI(mainTAG, "startup_task: begin");

    // Serial can still be used, but prefer ESP_LOGI for early logs
    // 1) PSRAM free already printed earlier in app_init, but re-log
    size_t free_psram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    ESP_LOGI(mainTAG, "startup_task: PSRAM Free: %llu", (unsigned long long)free_psram);

    // 2) allocate ring flat buffer in PSRAM
    RING_PAYLOAD_FLAT = AllocPSRamArray<uint32_t>(RING_FLAT_LEN, "RING_PAYLOAD_FLAT");
    if (!RING_PAYLOAD_FLAT) {
        ESP_LOGE(mainTAG, "startup_task: PSRAM::RING_PAYLOAD_FLAT allocation failed");
        // stop here - make obvious failure
        vTaskDelay(pdMS_TO_TICKS(1000));
        vTaskDelete(nullptr);
        return;
    }
    ESP_LOGI(mainTAG, "startup_task: RING_PAYLOAD_FLAT=%p len=%u", (void*)RING_PAYLOAD_FLAT, (unsigned)RING_FLAT_LEN);

    // 3) Set up microphone config (call user setter)
    user_mic_config_setter(miccfg);
    ESP_LOGI(mainTAG, "startup_task: miccfg set");

    // 4) keep same initialization but log each major step
    audio_rs_instance.nvsInitMain();
    ESP_LOGI(mainTAG, "startup_task: nvsInitMain done");

    bool wifiok = (WiFi.begin() == WL_CONNECTED);
    audio_rs_instance.set_WiFi_client_ptr(&WiFi_client);
    ESP_LOGI(mainTAG, "startup_task: WiFi.begin() returned=%d", wifiok);

    // create atomics
    g_consumer_ready = make_shared_atomic_bool(false);
    g_ring_head = make_shared_atomic_size_t(0);
    g_ring_tail = make_shared_atomic_size_t(0);
    g_abs_idx = make_shared_atomic_uint64_t(0);
    g_sequence_counter = make_shared_atomic_uint32_t(0);
    ESP_LOGI(mainTAG, "startup_task: atomics created");

    // create spans from allocated buffer
    std::span<uint32_t> i2s_span(I2S_WORD_SLOTS, I2S_WORD_SLOTS_LEN);
    std::span<uint32_t> ring_span = make_ring_flat_span();

    // setters (log each)
    audio_rs_instance.set_i2s_buffer(i2s_span);
    ESP_LOGI(mainTAG, "startup_task: set_i2s_buffer");

    audio_rs_instance.set_ring_payload_flat(ring_span, FRAMES_PER_PACKET);
    ESP_LOGI(mainTAG, "startup_task: set_ring_payload_flat");

    audio_rs_instance.set_ring_metadata_spans(ring_frames_span, ring_first_index_span, ring_ts_us_span);
    ESP_LOGI(mainTAG, "startup_task: set_ring_metadata_spans");

    audio_rs_instance.set_consumer_ready(g_consumer_ready);
    audio_rs_instance.set_ring_head(g_ring_head);
    audio_rs_instance.set_ring_tail(g_ring_tail);
    audio_rs_instance.set_abs_idx(g_abs_idx);
    audio_rs_instance.set_sequence_counter(g_sequence_counter);
    ESP_LOGI(mainTAG, "startup_task: shared pointers set");

    audio_rs_instance.set_micfg(miccfg);
    ESP_LOGI(mainTAG, "startup_task: miccfg applied");

    // Attach reciver config pointer but DO NOT call begin() or StartConfigPortal here
    // to avoid blocking during debug. We'll enable later once audio tasks are stable.
    recivercfg.setAudioRsPtr(&audio_rs_instance);
    audio_rs_instance.set_reciver_config_ptr(&recivercfg);
    ESP_LOGI(mainTAG, "startup_task: recivercfg ptr set");

    // OPTIONAL: comment out these if you want to isolate
    // recivercfg.begin();
    // recivercfg.AttachResetButton(RESET_WIFI_BUTTON_PIN);

    // init I2S and verify
    if (!audio_rs_instance.initI2S()) {
        ESP_LOGE(mainTAG, "startup_task: initI2S failed");
        // continue for testing or exit early
    } else {
        ESP_LOGI(mainTAG, "startup_task: initI2S succeeded");
    }

    // create network queue (if not already created inside class) --
    // ensure audio_rs_instance creates necessary queues in its init path.
    ESP_LOGI(mainTAG, "startup_task: about to start audio tasks");

    // Start tasks one-by-one and log returns
    TaskHandle_t h = nullptr;
    bool ok;

    ok = audio_rs_instance.start_task("I2SReaderLoop",
        DEFAULT_STACK_I2SReadTask, DEFAULT_PRIO_I2SReadTask, DEFAULT_CORE_I2SReadTask,
        AUDIO_RS::I2SReadTrampoline, &audio_rs_instance, &h);
    ESP_LOGI(mainTAG, "startup_task: I2SRead start returned=%d handle=%p", ok, (void*)h);

    vTaskDelay(pdMS_TO_TICKS(300)); // give it a moment

    ok = audio_rs_instance.start_task("RingWriterLoop",
        DEFAULT_STACK_RingWriterTask, DEFAULT_PRIO_RingWriterTask, DEFAULT_CORE_RingWriterTask,
        AUDIO_RS::RingWriterFRMI2STrampoline, &audio_rs_instance, &h);
    ESP_LOGI(mainTAG, "startup_task: RingWriter start returned=%d handle=%p", ok, (void*)h);

    vTaskDelay(pdMS_TO_TICKS(300));

    ok = audio_rs_instance.start_task("NetworkTaskLoop",
        DEFAULT_STACK_NetworkTask, DEFAULT_PRIO_NetworkTask, DEFAULT_CORE_NetworkTask,
        AUDIO_RS::NetworkTaskLoopTrampoline, &audio_rs_instance, &h);
    ESP_LOGI(mainTAG, "startup_task: NetworkTask start returned=%d handle=%p", ok, (void*)h);

    vTaskDelay(pdMS_TO_TICKS(300));

    ok = audio_rs_instance.start_task("NetworkDataWriterLoop",
        DEFAULT_STACK_NetworkDataWriterTask, DEFAULT_PRIO_NetworkDataWriterTask, DEFAULT_CORE_NetworkDataWriterTask,
        AUDIO_RS::NetworkDataWriterLoopTrampoline, &audio_rs_instance, &h);
    ESP_LOGI(mainTAG, "startup_task: NetworkWriter start returned=%d handle=%p", ok, (void*)h);

    ESP_LOGI(mainTAG, "startup_task: all start_task calls done; deleting startup_task now");
    vTaskDelete(nullptr);
}

void setup()
{
    // minimal main setup: start Serial + the heavy startup task
    Serial.begin(115200);
    delay(200);
    ESP_LOGI(mainTAG, "setup: launching startup_task");
    // create startup_task with big stack: 16K
    xTaskCreatePinnedToCore(startup_task, "startup", 16384, nullptr, tskIDLE_PRIORITY + 2, nullptr, 1);
}

extern "C" void arduino_setup_call() {
    setup();
}