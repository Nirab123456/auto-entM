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
    return std::span<uint32_t>(&RING_PAYLOAD[0][0], RING_FLAT_LEN);
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

void setup()
{
    Serial.begin(115200);
    delay(200);
    ESP_LOGD(mainTAG, "Starting........");

    size_t free_psram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    ESP_LOGI(mainTAG, "PSRAM Free: %llu",(unsigned long long)free_psram);


    RING_PAYLOAD_FLAT = AllocPSRamArray<uint32_t>(RING_FLAT_LEN, "RING_PAYLOAD_FLAT");
    if (!RING_PAYLOAD_FLAT)
    {
        ESP_LOGE(mainTAG, "PSRAM::RING_PAYLOAD_FLAT::Allocation failed");
        while (true)
        {
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
    }

    if (!I2S_WORD_SLOTS)
    {
        ESP_LOGE(mainTAG, "I2S_WORD_SLOTS->not available");
        heap_caps_free(RING_PAYLOAD_FLAT);
        while (true)
        {
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
    }

    //Set user microphone configuration
    user_mic_config_setter(miccfg);

    //start initialization
    audio_rs_instance.nvsInitMain();
    bool wifiok = (WiFi.begin() == WL_CONNECTED);
    audio_rs_instance.set_WiFi_client_ptr(&WiFi_client);

    //create atomics
    g_consumer_ready = make_shared_atomic_bool(false);
    g_ring_head = make_shared_atomic_size_t(0);
    g_ring_tail = make_shared_atomic_size_t(0);
    g_abs_idx = make_shared_atomic_uint64_t(0);
    g_sequence_counter = make_shared_atomic_uint32_t(0);

    //create spans from allocated buffer
    std::span<uint32_t> i2s_span(I2S_WORD_SLOTS, I2S_WORD_SLOTS_LEN);
    std::span<uint32_t> ring_span = make_ring_flat_span();

    //setters
        //spans
    audio_rs_instance.set_i2s_buffer(i2s_span);
    audio_rs_instance.set_ring_payload_flat(ring_span, FRAMES_PER_PACKET);
            //meta deta
    audio_rs_instance.set_ring_metadata_spans(ring_frames_span, ring_first_index_span, ring_ts_us_span);
        //shared pointers
    audio_rs_instance.set_consumer_ready(g_consumer_ready);
    audio_rs_instance.set_ring_head(g_ring_head);
    audio_rs_instance.set_ring_tail(g_ring_tail);
    audio_rs_instance.set_abs_idx(g_abs_idx);
    audio_rs_instance.set_sequence_counter(g_sequence_counter);
        //MicrophoneConfig        
    audio_rs_instance.set_micfg(miccfg);
    //ReciverConfig setter
        //AUDIO_RS class ptr
    recivercfg.setAudioRsPtr(&audio_rs_instance);
    //set reciver config - atleast wifi
    if (wifiok)
    {
        //begain ReciverConfig and pass to AUDIO_RS
        recivercfg.begin();
        audio_rs_instance.set_reciver_config_ptr(&recivercfg);
        if (audio_rs_instance.ReqNetworkReconnect())
        {
            ESP_LOGI(mainTAG, "MAIN::setup:Reciver connected");
        }
        else
        {
            ESP_LOGD(mainTAG, "MAIN::setup:Reciver not connected");  
        }
    }
    else
    {
        //start conf portal 
        recivercfg.StartConfigPortal(true);
    }
    
    //attach button
    bool attach_ok = recivercfg.AttachResetButton(RESET_WIFI_BUTTON_PIN);
    if (!attach_ok)
    {
        ESP_LOGE(mainTAG, "MAIN::ReciverConfig->AttachResetButton:Failed");
    }
    
    //initiate i2s peripherals
    if (!audio_rs_instance.initI2S())
    {
        ESP_LOGE(mainTAG, "MAIN::setup->AUDIO_RS:initI2S:Failed");
    }
    
    //START ALL TASKS
    TaskHandle_t h;
    audio_rs_instance.start_task(
        "I2SReaderLoop", DEFAULT_STACK_I2SReadTask, DEFAULT_PRIO_I2SReadTask, DEFAULT_CORE_I2SReadTask,
        AUDIO_RS::I2SReadTrampoline, &audio_rs_instance, &h
    );
    audio_rs_instance.start_task(
        "RingWriterLoop", DEFAULT_STACK_RingWriterTask, DEFAULT_PRIO_RingWriterTask, DEFAULT_CORE_RingWriterTask,
        AUDIO_RS::RingWriterFRMI2STrampoline, &audio_rs_instance, &h
    );
    audio_rs_instance.start_task(
        "NetworkTaskLoop", DEFAULT_STACK_NetworkTask, DEFAULT_PRIO_NetworkTask, DEFAULT_CORE_NetworkTask,
        AUDIO_RS::NetworkTaskLoopTrampoline, &audio_rs_instance, &h
    );   
    audio_rs_instance.start_task(
        "NetworkDataWriterLoop", DEFAULT_STACK_NetworkDataWriterTask, DEFAULT_PRIO_NetworkDataWriterTask, DEFAULT_CORE_NetworkDataWriterTask,
        AUDIO_RS::NetworkDataWriterLoopTrampoline, &audio_rs_instance, &h
    );

}

extern "C" void arduino_setup_call() {
    setup();
}