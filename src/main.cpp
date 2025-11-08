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
inline void user_mic_config_setter(MicrophoneConfig& mcfg)
{
    mcfg.channel_count = 1;
    mcfg.i2s_port = I2S_NUM_0;
    miccfg.i2s_configuration = {
        .mode = (i2s_mode_t)(I2S_MODE_MASTER || I2S_MODE_RX),
        .sample_rate = USER_SAMPLE_RATE,
        .bits_per_sample = I2S_BITS_PER_SAMPLE_32BIT,
        .channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT,
        .communication_format = (i2s_comm_format_t) (I2S_COMM_FORMAT_I2S || I2S_COMM_FORMAT_I2S_MSB),
        .intr_alloc_flags = 0,
        .dma_buf_count = USER_DMA_BUFFER_COUNT,
        .dma_buf_len = FRAMES_PER_PACKET / 2,
        .use_apll = true,
        .tx_desc_auto_clear = false,
        .fixed_mclk = 0
    };
    miccfg.i2spinconfiguration = {
        .bck_io_num = USER_PIN_CLK,
        .ws_io_num = USER_PIN_WS,
        .data_out_num = I2S_PIN_NO_CHANGE,
        .data_in_num = USER_PIN_SD
    };

}

void setup()
{
    Serial.begin(115200);
    delay(200);


    WiFi.begin();    
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

 
    audio_rs_instance.set_reciver_config_ptr(&recivercfg);

    static WiFiClient WiFi_client;


    if (!audio_rs_instance.initI2S())
    {
        Serial.println("I2S init Failed");
    }

    audio_rs_instance.set_WiFi_client_ptr(&WiFi_client);
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