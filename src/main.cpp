#include <Arduino.h>
#include <WiFi.h>
#include <WiFiManager.h>   // Install via Library Manager (look for "WiFiManager")
#include "driver/i2s.h"

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

const uint8_t HEADER_SIZE = 34;
const uint32_t HEADER_MAGIC = 0x45535032;
const uint8_t FORMAT_INT32_LEFT24 =1;

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

void setup()
{
    // put your setup code here, to run once:
    Serial.begin(115200);


    wifiConnect();
}

void loop()
{
}
