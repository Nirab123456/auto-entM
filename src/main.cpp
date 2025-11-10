#include "main_helper.h"


const char* WIFI_SSID = "94 Pembroke Street - 2";
const char* WiFi_PASS = "welcomehome";


std::shared_ptr<std::atomic<bool>> g_consumer_ready;
std::shared_ptr<std::atomic<size_t>> g_ring_head;
std::shared_ptr<std::atomic<size_t>> g_ring_tail;
std::shared_ptr<std::atomic<uint64_t>> g_abs_idx;
std::shared_ptr<std::atomic<uint32_t>> g_sequence_counter;

static ReciverConfig recivercfg;

static AUDIO_RS audio_rs_instance;
MicrophoneConfig miccfg;

void setup()
{
    bool wifiok = false;
    bool reconok = false;

    Serial.begin(115200);
    delay(200);
    Serial.println("Starting up...");

    wifiok = WiFi.begin();
    static WiFiClient WiFi_client;
    audio_rs_instance.set_WiFi_client_ptr(&WiFi_client);

    recivercfg.begin();
    unsigned long started = millis();
    const unsigned long timeout_ms = 10000;
    while (
        WiFi.status() != WL_CONNECTED &&
        (millis() - started) < timeout_ms 
    )
    {
        vTaskDelay(pdMS_TO_TICKS(200));
    }
    if (WiFi.status() == WL_CONNECTED)
    {
        Serial.println("WiFi connected");
    } else {
        Serial.println("WiFi not connected (continuing — network tasks will retry)");
    }

    //make shared atomic (night task and test of audio reciver)
    g_consumer_ready = make_shared_atomic_bool(false);
    g_ring_head = make_shared_atomic_size_t(0);
    g_ring_tail = make_shared_atomic_size_t(0);
    g_abs_idx = make_shared_atomic_uint64_t(0);
    g_sequence_counter = make_shared_atomic_uint32_t(0);


    std::span<uint32_t> i2s_span(I2S_WORD_SLOTS, I2S_WORD_SLOTS_LEN);
    std::span<uint32_t> ring_span = make_ring_flat_span();
    audio_rs_instance.set_i2s_buffer(i2s_span);
    audio_rs_instance.set_ring_payload_flat(ring_span, FRAMES_PER_PACKET);

    audio_rs_instance.set_consumer_ready(g_consumer_ready);
    audio_rs_instance.set_ring_head(g_ring_head);
    audio_rs_instance.set_ring_tail(g_ring_tail);
    audio_rs_instance.set_abs_idx(g_abs_idx);
    audio_rs_instance.set_sequence_counter(g_sequence_counter);

    audio_rs_instance.set_ring_metadata_spans(ring_frames_span, ring_first_index_span, ring_ts_us_span);

    user_mic_config_setter(miccfg);
    audio_rs_instance.set_micfg(miccfg);


    recivercfg.setAudioRsPtr(&audio_rs_instance);
    if (wifiok)
    {
        audio_rs_instance.set_reciver_config_ptr(&recivercfg);
        reconok = audio_rs_instance.ReqNetworkReconnect();
    }
    
    if (reconok)
    {
        recivercfg.StartConfigPortal();
    }
    else
    {
        recivercfg.StartConfigPortal(true);
    }
    
    
    bool attach_ok = recivercfg.AttachResetButton(RESET_WIFI_BUTTON_PIN, 20, 800, 3, 3072, -99, nullptr);
    if (!attach_ok)
    {
        Serial.println("main -> ReciverConfig::AttachResetButton:Failed");
    }
    else
    {
        if (ReciverConfig::ConfSRButtonTaskHandle_) //static ConfSRButtonTaskHandle_
        {
            audio_rs_instance.conf_portal_rst_button_handler_ = ReciverConfig::ConfSRButtonTaskHandle_;
        }
        
    }
    
    audio_rs_instance.set_reciver_config_ptr(&recivercfg);


    if (!audio_rs_instance.initI2S())
    {
        Serial.println("I2S init Failed");
    }

    // 8) Start tasks (trampolines already declared in header)
    TaskHandle_t h;
    audio_rs_instance.start_task("I2SRead", 4096, 2, 1, AUDIO_RS::I2SReadTrampoline, &audio_rs_instance, &h);
    audio_rs_instance.start_task("RingWriter", 8192, 2, 1, AUDIO_RS::RingWriterFRMI2STrampoline, &audio_rs_instance, &h);
    audio_rs_instance.start_task("NetworkTask", 8192, 2, 1, AUDIO_RS::NetworkTaskLoopTrampoline, &audio_rs_instance, &h);
    audio_rs_instance.start_task("NetWriter", 8192, 2, 1, AUDIO_RS::NetworkDataWriterLoopTrampoline, &audio_rs_instance, &h);

    // no console input — ReciverConfig determines where we connect (from stored preferences)
    // Optionally you can programmatically update recfg.save("192.168.x.y", port) elsewhere (OTA/UI).
}

void loop() {
    // Idle; tasks do the work.
    vTaskDelay(pdMS_TO_TICKS(5000));
}