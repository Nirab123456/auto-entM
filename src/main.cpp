#include <Arduino.h>
#include <WiFi.h>
#include <WiFiManager.h>   // Install via Library Manager (look for "WiFiManager")
#include "driver/i2s.h"

const uint8_t S_B_C = 4; //standered byte size 

// wifi configuration (defaults - will be overridden by WiFiManager if you set new creds)
const char* WIFI_SSID = "94 Pembroke Street - 2";
const char* WIFI_PAS = "welcomehome";
const char* PC_IP = "192.168.2.133";
const uint16_t PC_PORT = 7000;

// i2s pins
const uint8_t PIN_CLK = 7;
const uint8_t PIN_WS = 15;
const uint8_t PIN_SD = 16;

const uint16_t SAMPLE_RATE = 48000;
const i2s_bits_per_sample_t I2S_BITS = I2S_BITS_PER_SAMPLE_32BIT;
const uint8_t NUMBER_CHANNELS = 1;
const uint16_t FRAMES_PER_PACKET = 1024;
const uint8_t BYTES_PER_SAMPLE = 4;
const size_t BYTES_TO_READ = (size_t)(FRAMES_PER_PACKET*BYTES_PER_SAMPLE*2);
const size_t PAYLOAD_BYTES = (size_t)(FRAMES_PER_PACKET*BYTES_PER_SAMPLE*NUMBER_CHANNELS);
const size_t NEEDED_WORDS = (size_t)(FRAMES_PER_PACKET*2);


const uint8_t HEADER_SIZE = 34;
const uint32_t HEADER_MAGIC = 0x45535032;
const uint8_t FORMAT_INT32_LEFT24 =1;
uint8_t header[HEADER_SIZE];

WiFiClient TCPCLIENT;

volatile uint32_t sequense_counter = 0;
uint64_t absolute_sample_index = 0;

static uint32_t i2s_word_slots[FRAMES_PER_PACKET*2];
static uint32_t payload_words [FRAMES_PER_PACKET];

// Use WiFiManager to allow dynamic SSID / password set through captive portal
void wifiConnectmanager() {
    WiFi.mode(WIFI_STA); // explicitly set mode, esp defaults to STA+AP
    WiFiManager wm;
 
    bool res;
    // res = wm.autoConnect(); // auto generated AP name from chipid
    // res = wm.autoConnect("AutoConnectAP"); // anonymous ap
    res = wm.autoConnect("AutoConnectAP","password"); // password protected ap
 
    if(!res) {
        Serial.println("Failed to connect");
        // ESP.restart();
    } 
    else {
        //if you get here you have connected to the WiFi    
        Serial.println("connected...yeey :)");
    }
}

void i2s_init()
{
    i2s_config_t i2s_configaration ={
        .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX),
        .sample_rate = SAMPLE_RATE,
        .bits_per_sample = I2S_BITS,
        .channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT,
        .communication_format =(i2s_comm_format_t)(I2S_COMM_FORMAT_I2S | I2S_COMM_FORMAT_I2S_MSB),
        .intr_alloc_flags = 0,
        .dma_buf_count = 6,
        .dma_buf_len = FRAMES_PER_PACKET / 2,
        .use_apll = true,
        .tx_desc_auto_clear = false,
        .fixed_mclk = 0

    };

    i2s_pin_config_t pin_configaration = {
        .bck_io_num = PIN_CLK, // BCK is CLK
        .ws_io_num = PIN_WS,
        .data_out_num = I2S_PIN_NO_CHANGE,
        .data_in_num = PIN_SD, // SD::SERIAL DATA is Data
    };

    esp_err_t i2s_install = i2s_driver_install(I2S_NUM_0,&i2s_configaration,0,NULL);

    if (i2s_install != ESP_OK)
    {
        Serial.printf("[I2S] driver_install failed: %d\n", i2s_install);
        while (1)
        {
            delay(500);
        }
        
    }
    i2s_install = i2s_set_pin(I2S_NUM_0,&pin_configaration);
    if (i2s_install != ESP_OK)
    {
        Serial.printf("[I2S] driver_install failed: %d\n", i2s_install);
        while (1)
        {
            delay(500);
        }
        
    }
    i2s_zero_dma_buffer(I2S_NUM_0);
    Serial.println("[I2S] initialized (APLL enabled)");
}
int write_tcp_header(
                    uint32_t seq,
                    uint64_t first_sample_index,
                    uint64_t timestamp_us,
                    uint16_t number_of_frames
                        )
{
    //MAGIC
    header[0] = ((uint8_t)(HEADER_MAGIC&0xff));
    header[1] = ((uint8_t)((HEADER_MAGIC >>8)&0xff));
    header[2] = ((uint8_t)((HEADER_MAGIC >> 16)&0xff));
    header[3] = ((uint8_t)((HEADER_MAGIC >> 24)&0xff));

    //sequense(4-7) sample rate(28-31)
    for (int i = 0; i < S_B_C; ++i) 
    {
        header[4+i] = ((uint8_t)(seq>>(i*0)&0xff));
        header[8+i] = ((uint8_t)(SAMPLE_RATE>>(8*i)&0xff));
    }
    
    for (size_t i = 0; i < S_B_C *2; i++)
    {
        header[12+i] = ((uint8_t)((first_sample_index >> 8*i)&0xff));
        header[20+i] = ((uint8_t)((timestamp_us >> 8 *i)&0xff));
    }
    header[28] = ((uint8_t)number_of_frames&0xff);
    header[29] = ((uint8_t)((number_of_frames>>8)&0xff));
    header[30] = (uint8_t) NUMBER_CHANNELS;
    header[31] = (uint8_t) BYTES_PER_SAMPLE;
    header[32] = (uint8_t) (FORMAT_INT32_LEFT24 & 0xff);
    header[33] = (uint8_t) ((FORMAT_INT32_LEFT24 >> 8)&0xff);


    return 1;


}

bool send_tcp_packet(WiFiClient &client,const uint32_t* payload_words_pointer)
{
    if (!client || !client.connected())
    {
        return false;
    }

    size_t hsent = 0;
    while (hsent < HEADER_SIZE)
    {
        int written_header = client.write(header+hsent,HEADER_SIZE-hsent);
        if (written_header <=0)
        {
            return false;
        }
        hsent += (size_t) written_header;
        
    }
    const uint8_t* p = (const uint8_t*)payload_words_pointer; // payload_words_ptr is an array of uint32
    size_t sent_payload = 0;
    while (sent_payload < PAYLOAD_BYTES)
    {
        int written_payload = client.write(p+sent_payload,PAYLOAD_BYTES-sent_payload);
        if (written_payload <= 0)
        {
            return false;
        }
        sent_payload += (size_t)written_payload;
        if (sent_payload%1024==0)
        {
            taskYIELD();
        }
        return true;   
    }
}

void audiotask(void *pv)
{
    (void)pv;

    Serial.printf("[TASK] starting audioTask: FRAMES=%u bytesToRead=%u payload=%u\n",
                    FRAMES_PER_PACKET, (unsigned)BYTES_TO_READ, (unsigned)PAYLOAD_BYTES);

    while (true)
    {
        if (!TCPCLIENT || !TCPCLIENT.connected())
        {
            Serial.println("Connecting to TCP server");
            if (TCPCLIENT.connect(PC_IP,PC_PORT))
            {
                TCPCLIENT.setNoDelay(true);
                Serial.println("TCP connected");
            }
            else
            {
                Serial.println("[TCP] connect failed, retry in 1s");
                delay(1000);
                continue;
            }
            
        }
        
        size_t bytes_read = 0;
        esp_err_t resolve = i2s_read(I2S_NUM_0,(void*)i2s_word_slots,BYTES_TO_READ,&bytes_read,portMAX_DELAY);
        if (resolve != ESP_OK || bytes_read == 0)
        {
            Serial.printf("[I2S] read err %d bytes=%u\n", resolve, (unsigned)bytes_read);
            // brief delay to avoid tight loop on persistent error
            delay(10);
            continue;      
        }

        size_t word_count = bytes_read/4; // will be replaced the magic number with proper variable
        size_t available_frames =0;
        if (word_count >= NEEDED_WORDS)
        {
            for (size_t i = 0; i < FRAMES_PER_PACKET; i++)
            {
                payload_words[i] = i2s_word_slots[i*2+1];
            }
            available_frames = FRAMES_PER_PACKET;
            
        }
        else
        {
            size_t maxframes = word_count / 2;
            for (size_t i = 0; i < maxframes; i++)
            {
                payload_words[i] = i2s_word_slots[i*2+1];
            }
            for (size_t i = maxframes; i < FRAMES_PER_PACKET; i++)
            {
                payload_words[i] = 0;
            }
            available_frames = maxframes;            
        }
        
        uint32_t seq = ++sequense_counter;
        uint64_t first_sample_index = absolute_sample_index;
        uint64_t timestamp_us = (uint64_t)esp_timer_get_time();

        int write_header_ok = write_tcp_header(seq,first_sample_index,timestamp_us,(uint16_t)available_frames);

        while (write_header_ok !=1)
        {
            Serial.println("Failure in writing Header");
            write_header_ok = write_tcp_header(seq,first_sample_index,timestamp_us,(uint16_t)available_frames);

        }
        
        bool send_packet_ok = send_tcp_packet(TCPCLIENT,payload_words);
        if (!send_packet_ok)
        {
            Serial.println("Failure in data communication (SENDING)--TCP");
            TCPCLIENT.stop();
            delay(50);
            continue;
        }
        absolute_sample_index += (uint64_t)available_frames;
        taskYIELD();
    }
    vTaskDelete(NULL);
}

void STARTAUDIOTASK()
{
    xTaskCreatePinnedToCore(audiotask,"audiotask",8192,NULL,6,NULL,1);
}

void setup()
{
    // put your setup code here, to run once:
    Serial.begin(115200);
    while (!Serial)
    {
        delay(5);
    }

    Serial.println("\n=== ESP32 I2S -> TCP streamer (high-quality, sample-indexed) ===");



    wifiConnectmanager();
    i2s_init();
    STARTAUDIOTASK();
    Serial.println("setup done :)");
}

void loop()
{
  // Main loop prints status occasionally
  static unsigned long last = 0;
  if (millis() - last > 2000) {
    last = millis();
    Serial.printf("[STAT] WiFi=%s TCP=%s seq=%u sample_idx=%llu\n",
                  WiFi.isConnected() ? "OK" : "NO",
                  TCPCLIENT.connected() ? "OK" : "NO",
                  (unsigned)sequense_counter,
                  (unsigned long long)absolute_sample_index);
  }
  delay(100);}
