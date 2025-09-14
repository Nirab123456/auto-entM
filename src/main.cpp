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
void wifiConnect() {
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
void write_tcp_header(
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


}

bool send_tcp_packet(WiFiClient &client,const uint32_t* payload_words_pointer,size_t payload_bytes)
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
    while (sent_payload < payload_bytes)
    {
        int written_payload = client.write(p+sent_payload,payload_bytes-sent_payload);
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

    }
    
    
}


void setup()
{
    // put your setup code here, to run once:
    Serial.begin(115200);


    wifiConnect();
}

void loop()
{
}
