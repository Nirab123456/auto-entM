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
const uint8_t DEFAULT_CHANNEL_COUNT = 2;
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
static uint32_t RING_PAYLOAD[RING_SIZE][FRAMES_PER_PACKET];
static uint64_t RING_TIMESTAMP[RING_SIZE];
static uint16_t RING_FRAMES[RING_SIZE];
static uint64_t RING_FIRST_INDEX [RING_SIZE];


static ReciverConfig globalreciver_config;


//atomic variables
std::atomic<size_t> Ring_head{0};
std::atomic<size_t> Ring_tail{0};
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
        String  saved_port = prefs_.getString("pc_port","");
        prefs_.end();
        IPAddress tmp;
        if (saved_ip.length()&& tmp.fromString(saved_ip))
        {
            ip_ = tmp;

        }
        else{
            ip_ = IPAddress(0,0,0,0);
        }
        if (saved_port.length())
        {
            long p = saved_port.toInt();
            port_ = (p>0 && p <= 65535) ? (uint16_t)p: 0;
        }
        else{
            port_ = 0;
        }
    }
    void save(const char* ip_str, uint16_t port)
    {
        std::lock_guard<std::mutex> lock(mu_);
        prefs_.begin(PREF_NAMESPACE,false);
        prefs_.putString("pc_ip",String(ip_str));
        prefs_.putString("pc_port",String((unsigned)port));
        prefs_.end();
        IPAddress tmp;
        if (ip_str && ip_str[0] && tmp.fromString(String(ip_str)))
        {
            ip_ = tmp;
            port_ = port;
        }
    }
    void clear()
    {
        std::lock_guard<std::mutex> lock(mu_);
        prefs_.begin(PREF_NAMESPACE,false);
        prefs_.remove("pc_ip");
        prefs_.remove("pc_port");
        prefs_.end();
        ip_ = IPAddress(0,0,0,0);
        port_ = 0;
    }
    void get(IPAddress &out_ip, uint16_t &out_port)
    {
        std::lock_guard<std::mutex> lock(mu_);
        out_ip = ip_;
        out_port = port_;
    }   
    String ipString()
    {
        std::lock_guard<std::mutex> lock(mu_);
        if (ip_ == IPAddress(0,0,0,0))
        {
            return String("(none)");
        }
        return ip_.toString();
        
    }
    unsigned short port()
    {
        std::lock_guard<std::mutex> lock(mu_);
        return port_;
    }
    bool isvalid()
    {
        std::lock_guard<std::mutex> lock(mu_);
        if (ip_ != IPAddress(0,0,0,0)&& port_ != 0)
        {
            return true;
        }
        return false;
        
    }

};

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
    String ip_default = globalreciver_config.ipstring();
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


void networktask(void* pv)
{

}


void starttask()
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
    starttask();
    Serial.println("RESET: RESET DONE");
}
