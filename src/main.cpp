//headers
#include "audio_conf_sett.h"

#include "audio_helper.h"
#include "ReciverConfig.h"

// ---------- buffers (single definitions) ----------
uint8_t HEADER_BUFFER[HEADER_SIZE];

uint32_t I2S_WORD_SLOTS[FRAMES_PER_PACKET * 2];
uint32_t payload_words[FRAMES_PER_PACKET];

// ring buffers
uint32_t RING_PAYLOAD[RING_SIZE][FRAMES_PER_PACKET];
uint64_t RING_TIMESTAMP[RING_SIZE];
uint16_t RING_FRAMES[RING_SIZE];
uint64_t RING_FIRST_INDEX[RING_SIZE];

// ---------- atomic/shared state (single definitions) ----------
std::atomic<size_t> Ring_head{0};
std::atomic<size_t> Ring_tail{0};
std::atomic<uint32_t> Sequence_counter{0};
std::atomic<uint64_t> Absolute_sample_index{0};
std::atomic<bool> consumer_ready{false};

// ---------- task handles (single definitions) ----------
TaskHandle_t audiohandleTASK = NULL;
TaskHandle_t networkhandleTASK = NULL;
TaskHandle_t printtaskHANDLE = NULL;
TaskHandle_t monitorhandleTASK = NULL;

SemaphoreHandle_t button_semaphore = NULL;
volatile TickType_t isr_press_tick = 0;
volatile TickType_t isr_last_edge_tick = 0;

WiFiClient TCPCLIENT;




static ReciverConfig globalreciver_config;

void clear_ring_nd_rst_indices()
{
    Ring_head.store(0, std::memory_order_relaxed);
    Ring_tail.store(0,std::memory_order_relaxed);
    for (size_t i = 0; i < RING_SIZE; i++)
    {
        RING_FRAMES[i] = 0;
        RING_FIRST_INDEX[i] = 0;
        RING_TIMESTAMP[i] = 0 ;
    }
    Sequence_counter.store(0,std::memory_order_relaxed);
    Absolute_sample_index.store(0,std::memory_order_relaxed);
    Serial.printf("(RING): Cleared metadata\nSample indices (RING SIZE : %u)\n",((unsigned)RING_SIZE));
}

void setup_wifi_and_params(){
    globalreciver_config.load();
    String ip_default = globalreciver_config.ipString();
    char ip_buffer[40];
    ip_buffer[0] =0;
    ip_default.toCharArray(ip_buffer,sizeof(ip_buffer));
    unsigned short port_data = globalreciver_config.port();
    char port_buffer[16];
    port_buffer[0] = 0;
    if (port_data)
    {
        snprintf(port_buffer,sizeof(port_buffer),"%u",(unsigned)port_data);
    }
    WiFiManager wm_local;
    WiFiManagerParameter ip_parameaters("pc_ip","",ip_buffer,sizeof(ip_buffer));
    WiFiManagerParameter port_parameters("pc_port","",port_buffer,sizeof(port_buffer));
    wm_local.addParameter(&ip_parameaters);
    wm_local.addParameter(&port_parameters);

    Serial.println("WIFI: launching WIFIMANAGER autoconnect \n Portal will show ip and port if available.");
    bool resolved = wm_local.autoConnect(WIFI_AP_NAME,WIFI_AP_PASS);
    if(!resolved)
    {
        Serial.println("[WIFI] WiFiManager autoConnect failed or canceled");
        consumer_ready.store(false,std::memory_order_release);
        return;
    }

    const char* entered_ip = ip_parameaters.getValue();
    const char* entered_port = port_parameters.getValue();

    if (entered_ip && entered_ip[0])
    {
        IPAddress tmp;
        if (tmp.fromString(String(entered_ip)))
        {
            unsigned p = 0;
            if (entered_port && entered_port[0])
            {
                p = (unsigned)atoi(entered_port);
            }
            if (p>0 && p < 65535)
            {
                globalreciver_config.save(entered_ip,(uint16_t)p);
                Serial.printf("[WIFI] saved PC IP=%s PORT=%u\n", entered_ip, (unsigned)p);
            }
            else{
                globalreciver_config.save(entered_ip,0);
                Serial.println("WIFI: IP saved BUT Not a valid port ");
            }
        }
        else
        {
            Serial.println("WIFI : NOT a valid IP");
            globalreciver_config.clear();   
        }
    }
    else
    {
        Serial.println("WIFI : No ip entered");
    }
    consumer_ready.store(false,std::memory_order_release);
}

void audiotask(void* pv)
{
    (void)pv;
    Serial.printf("TASK : AUDIOTASK \nFrames : %u Bytes to read : %u Payload : %u \n",FRAMES_PER_PACKET,(unsigned)BYTES_TO_READ,(unsigned)PAYLOAD_BYTES);
    bool paused = false;
    while (true)
    {
        if (!consumer_ready.load(std::memory_order_acquire))
        {
            if (!paused)
            {
                Serial.println("TASK -> NETWORK : No reciver connected");
                i2s_zero_dma_buffer(I2S_NUM_0);
                vTaskDelay(pdMS_TO_TICKS(200));
                continue;
            }
        }
        else
        {
            if (paused)
            {
                Serial.println("AUDIOTASK: Resuming.....");
                paused = false;
            }
        }

        size_t already_read_bites = 0;
        esp_err_t i2s_read_error = i2s_read(I2S_NUM_0,(void*)I2S_WORD_SLOTS,BYTES_TO_READ,&already_read_bites,portMAX_DELAY);

        if (i2s_read_error != ESP_OK || already_read_bites == 0)
        {
            Serial.printf("T2S : Read error\nAmount of bytes been read : %u \n",(unsigned)already_read_bites);
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
        size_t word_count = already_read_bites/BYTES_PER_SAMPLE;
        size_t available_frames = (word_count >= NEEDED_WORDS) ? FRAMES_PER_PACKET : (word_count/DEFAULT_CHANNEL_COUNT);
        if (available_frames >FRAMES_PER_PACKET)
        {
            available_frames = FRAMES_PER_PACKET;
        }
        size_t head = Ring_head.load(std::memory_order_relaxed);
        size_t tail = Ring_tail.load(std::memory_order_acquire);
        size_t nexthead = head +1;
        if ((nexthead-tail)>RING_SIZE)
        {
            Absolute_sample_index.fetch_add((uint64_t)available_frames,std::memory_order_relaxed);
            static unsigned drop_count = 0;
            if ((++drop_count%10)==0)
            {
                Serial.printf("AUDIO RING : BUFFER FULL- Amont of packet dropping : %u\nAvailable free heap = %u\n",(unsigned)(head-tail),(unsigned)esp_get_free_heap_size());
            }
            taskYIELD();
            continue;
        }
        size_t slot = head & RING_MASK;
        if (available_frames == FRAMES_PER_PACKET)
        {
            for (size_t i = 0; i < FRAMES_PER_PACKET; i++)
            {
                RING_PAYLOAD[slot][i] = I2S_WORD_SLOTS[i*2+1];
            }
        }
        else
        {
            for (size_t i = 0; i < available_frames; i++)
            {
                RING_PAYLOAD[slot][i]=I2S_WORD_SLOTS[i*2+1];
            }
            for (size_t i = available_frames; i < FRAMES_PER_PACKET; i++)
            {
                RING_PAYLOAD[slot][i] = 0;
            }
        }
        uint64_t first_s_index = Absolute_sample_index.load(std::memory_order_relaxed);
        uint64_t ts = (uint64_t)esp_timer_get_time();
        RING_FRAMES[slot] = (uint16_t)available_frames;
        RING_FIRST_INDEX[slot] = first_s_index;
        RING_TIMESTAMP[slot] = ts;
        Ring_head.store(nexthead,std::memory_order_release);
        Absolute_sample_index.fetch_add((uint64_t)available_frames,std::memory_order_relaxed);
        taskYIELD();
    }
    vTaskDelete(NULL);
}

void write_tcp_header(uint32_t seq, uint64_t first_sample_index, uint64_t timestamp_us, uint16_t number_of_frames) {
  HEADER_BUFFER[0] = (uint8_t)(HEADER_MAGIC & 0xFF);
  HEADER_BUFFER[1] = (uint8_t)((HEADER_MAGIC >> 8) & 0xFF);
  HEADER_BUFFER[2] = (uint8_t)((HEADER_MAGIC >> 16) & 0xFF);
  HEADER_BUFFER[3] = (uint8_t)((HEADER_MAGIC >> 24) & 0xFF);
  for (int i=0;i<4;++i) HEADER_BUFFER[4+i] = (uint8_t)((seq >> (8*i)) & 0xFF);
  for (int i=0;i<8;++i) HEADER_BUFFER[8+i] = (uint8_t)((first_sample_index >> (8*i)) & 0xFF);
  for (int i=0;i<8;++i) HEADER_BUFFER[16+i] = (uint8_t)((timestamp_us >> (8*i)) & 0xFF);
  HEADER_BUFFER[24] = (uint8_t)(number_of_frames & 0xFF);
  HEADER_BUFFER[25] = (uint8_t)((number_of_frames >> 8) & 0xFF);
  HEADER_BUFFER[26] = (uint8_t)NUMBERS_OF_CHANNELS;
  HEADER_BUFFER[27] = (uint8_t)BYTES_PER_SAMPLE;
  HEADER_BUFFER[28] = (uint8_t)(SAMPLE_RATE & 0xFF);
  HEADER_BUFFER[29] = (uint8_t)((SAMPLE_RATE >> 8) & 0xFF);
  HEADER_BUFFER[30] = (uint8_t)((SAMPLE_RATE >> 16) & 0xFF);
  HEADER_BUFFER[31] = (uint8_t)((SAMPLE_RATE >> 24) & 0xFF);
  HEADER_BUFFER[32] = (uint8_t)(FORMAT_INT32_LEFT24 & 0xFF);
  HEADER_BUFFER[33] = (uint8_t)((FORMAT_INT32_LEFT24 >> 8) & 0xFF);
}

static bool connect_to_reciver_ip(const IPAddress &ip, uint16_t port)
{
    if (ip==IPAddress(0,0,0,0) || port == 0)
    {
        return false;
    }

    TCPCLIENT.stop();
    delay(10);
    bool con_ok = TCPCLIENT.connect(ip,port);
    if (!con_ok || !TCPCLIENT.connected())
    {
        TCPCLIENT.stop();
        Serial.println("REXIVER : CONNECTION failed");
        return false;
    }
    TCPCLIENT.setNoDelay(true);
    Serial.print("Reciver : connected to ip : ");
    Serial.print(String(ip));
    Serial.print("\n");
    return true;
}


void networktask(void* pv)
{
    (void)pv;
    unsigned long now = millis();
    IPAddress remote_ip;
    uint16_t remote_port = 0;
    unsigned long last_conn_attempt = 0;

    while (true)
    {
        globalreciver_config.get(remote_ip,remote_port);
        if(!TCPCLIENT.connected())
        {
            unsigned long now = millis();
            if (globalreciver_config.isvalid()&&WiFi.isConnected()&& (now - last_conn_attempt >= 200))
            {
                last_conn_attempt = now;
                consumer_ready.store(false, std::memory_order_release);
                clear_ring_nd_rst_indices();
                if (connect_to_reciver_ip(remote_ip,remote_port))
                {
                    consumer_ready.store(true,std::memory_order_release);
                }
                else
                {
                    consumer_ready.store(false, std::memory_order_release);
                    vTaskDelay(pdMS_TO_TICKS(20));
                    continue;
                }
            }
        }
        size_t tail = Ring_tail.load(std::memory_order_acquire);
        size_t head = Ring_head.load(std::memory_order_acquire);
        if (tail==head)
        {
            //nothing to send
            vTaskDelay(pdMS_TO_TICKS(2));
            continue;
        }
        size_t slot = tail & RING_MASK;
        uint16_t frames  = RING_FRAMES[slot];
        if (frames == 0 || frames > FRAMES_PER_PACKET)
        {
            Ring_tail.store(tail+1,std::memory_order_release);
            continue;
        }
        uint32_t seq = Sequence_counter.fetch_add(1,std::memory_order_relaxed);
        uint64_t first_index = RING_FIRST_INDEX[slot];
        uint64_t ts = RING_TIMESTAMP[slot];
        write_tcp_header(seq,first_index,ts,(uint16_t)frames);
        
        size_t hsent = 0;
        bool ok = true;
        while (hsent<(size_t)HEADER_SIZE)
        {
            int written = TCPCLIENT.write(HEADER_BUFFER+hsent,HEADER_SIZE-hsent);
            if (written <= 0)
            {
                ok =false;
                break;
            }
            hsent += (size_t)written;
            size_t sent = 0;
            if ((sent%1024)== 0)
            {
                taskYIELD();
            }
        }
        if (!ok)
        {
            Serial.println("NET: Failed closing soket clewaning buffers");
            TCPCLIENT.stop();
            consumer_ready.store(false,std::memory_order_release);
            clear_ring_nd_rst_indices();
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }
        Ring_tail.store(tail+1,std::memory_order_release);
        taskYIELD();
    }
    vTaskDelete(NULL);
}


void startTASK()
{
    const uint32_t audiostake = 8192;
    const uint32_t netstack = 4096;
    if (audiohandleTASK==NULL)
    {
        if (xTaskCreatePinnedToCore(audiotask,"audiotask",audiostake,NULL,6,&audiohandleTASK,1)!=pdPASS)
        {
            Serial.println("AUDIOTASK : Creation failed");
            audiohandleTASK = NULL;
        }
        else
        {
            Serial.println("AUDIOTASK : Started .......");
        }
    }
    if (networkhandleTASK == NULL)
    {
        if (xTaskCreatePinnedToCore(networktask,"networktask",netstack,NULL,5,&networkhandleTASK,0)!=pdPASS)
        {
            Serial.println("Network : Creation failed");
            networkhandleTASK = NULL;
        }
        else
        {
            Serial.println("NETWORK: Task created...");
        }
    }
}

void stoptask()
{
    consumer_ready.store(false,std::memory_order_release);
    if (TCPCLIENT.connected())
    {
        TCPCLIENT.stop();
    }
    if (audiohandleTASK != NULL)
    {
        vTaskDelete(audiohandleTASK);
        audiohandleTASK = NULL;
        Serial.println("TASK :: AUDIOTASK :: Deleted");
    }
    if (networkhandleTASK != NULL)
    {
        vTaskDelete(networkhandleTASK);
        networkhandleTASK = NULL;
        Serial.println("TASK :: NETWORKTASK :: Deleted");
    }
    delay(50);
}

void startconfigportal_button()
{
    Serial.println("RESET: Clearing saved data starting new portal.......");
    stoptask();
    globalreciver_config.clear();
    WiFiManager wm_tmp;
    wm_tmp.resetSettings();
    WiFi.disconnect(true,true);
    WiFi.mode(WIFI_STA);
    delay(200);
    setup_wifi_and_params();
    clear_ring_nd_rst_indices();
    startTASK();
    Serial.println("RESET: RESET DONE");
}

void i2s_init()
{
    i2s_config_t i2s_config = {
        .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX),
        .sample_rate = SAMPLE_RATE,
        .bits_per_sample = I2S_BITS,
        .channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT,
        .communication_format = (i2s_comm_format_t)(I2S_COMM_FORMAT_I2S|I2S_COMM_FORMAT_I2S_MSB),
        .intr_alloc_flags = 0,
        .dma_buf_count = 6,
        .dma_buf_len = FRAMES_PER_PACKET / 2,
        .use_apll = true,
        .tx_desc_auto_clear = false,
        .fixed_mclk = 0
    };
    i2s_pin_config_t pin_config = {
        .bck_io_num = PIN_CLK,
        .ws_io_num = PIN_WS,
        .data_out_num = I2S_PIN_NO_CHANGE,
        .data_in_num = PIN_SD
    };
    esp_err_t err = i2s_driver_install(I2S_NUM_0,&i2s_config,0,NULL);
    if (err != ESP_OK)
    {
        Serial.printf("I2S : Driver instalation ");
        while (1)
        {
            delay(500);
        }
        
    }
    err = i2s_set_pin(I2S_NUM_0,&pin_config);
    if (err != ESP_OK)
    {
        Serial.println("I2S : SETUP PIN FAILED");
        while (1)
        {
            delay(500);
        }    
    }
    i2s_zero_dma_buffer(I2S_NUM_0);
    Serial.println("I2S : INITIALIZED (APLL = TRUE)");
}

void setup()
{
    Serial.begin(115200);

    Serial.println("SETUP : INETIALIZING");
    setup_button_isr_TASK();
    // pinMode(RESET_WIFI_BUTTON_PIN,INPUT_PULLUP);
    globalreciver_config.begin();
    setup_wifi_and_params();
    i2s_init();
    clear_ring_nd_rst_indices();
    consumer_ready.store(false);
    startTASK();
    startmonitorTASK();
    printhandleTASK();
    Serial.println("SETUP : COMPLEATED :-)");
    
}
void printTASK(void*pv)
{
    (void)pv;
    const TickType_t delayTicks = pdMS_TO_TICKS(PRINT_INTERVAL_MS);
    for (;;)
    {
        vTaskDelay(delayTicks);
        IPAddress ip;
        uint16_t port;
        globalreciver_config.get(ip,port);
        size_t head = Ring_head.load(std::memory_order_relaxed);
        size_t tail = Ring_tail.load(std::memory_order_relaxed);
        unsigned occupancy = 0;
        if (head>= tail)
        {
            occupancy = ((unsigned)(head-tail));
        }
        if (occupancy > (unsigned)RING_SIZE)
        {
            occupancy = (unsigned)RING_SIZE;
        }
        unsigned long long sample_index = (unsigned long long)Absolute_sample_index.load(
            std::memory_order_relaxed);
        String s;
        s.reserve(300);
        s+= "WIFI SETUP : ";
        s+=(WiFi.isConnected()?"ok":"no");
        s+= " Ring : ";
        s+=String(occupancy);
        s+= "/";
        s+=String((unsigned)RING_SIZE);
        s+= " Sample index : ";
        s+= String(sample_index);
        s+= " PC IP: ";
        s+= (ip? ip.toString():String("None"));
        s+= " port :";
        s+= String((unsigned)port);
        s+= " FreeHeap : ";
        s+= String((unsigned)esp_get_free_heap_size());
        s+= consumer_ready.load(std::memory_order_relaxed)? "Consumer : READY" : "Consumer : Inactive";
        Serial.println(s);
    }
}
void loop()
{
}


