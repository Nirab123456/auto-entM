// corrected_esp32_stream_tcp_i2s_wm.ino
#include <Arduino.h>
#include <WiFi.h>
#include <WiFiManager.h>   // Install via Library Manager
#include "driver/i2s.h"
#include "esp_timer.h"     // for esp_timer_get_time()
#include <Preferences.h>

// ---------- user configuration (now dynamic) ----------
String PC_IP = "";         // will be filled from WiFiManager / Preferences
uint16_t PC_PORT = 0;

const char* WIFI_AP_NAME = "AutoConnectAP";
const char* WIFI_AP_PASS = "password";

// i2s pins and capture params
const uint8_t PIN_CLK = 7;
const uint8_t PIN_WS  = 15;
const uint8_t PIN_SD  = 16;

const uint32_t SAMPLE_RATE = 48000;
const i2s_bits_per_sample_t I2S_BITS = I2S_BITS_PER_SAMPLE_32BIT;
const uint8_t NUMBER_CHANNELS = 1;
const uint16_t FRAMES_PER_PACKET = 1024;
const uint8_t BYTES_PER_SAMPLE = 4;

// derived sizes
const size_t BYTES_TO_READ = (size_t)FRAMES_PER_PACKET * BYTES_PER_SAMPLE * 2; // stereo slots
const size_t PAYLOAD_BYTES  = (size_t)FRAMES_PER_PACKET * BYTES_PER_SAMPLE * NUMBER_CHANNELS;
const size_t NEEDED_WORDS   = (size_t)FRAMES_PER_PACKET * 2;

// header constants (must match receiver)
constexpr int HEADER_SIZE = 34;
const uint32_t HEADER_MAGIC = 0x45535032; // 'ESP2'
const uint16_t FORMAT_INT32_LEFT24 = 1;

// globals
uint8_t header_buf[HEADER_SIZE];
WiFiClient TCPCLIENT;

// atomic for safe access from two contexts (task + main)
volatile uint32_t sequence_counter = 0;
uint64_t absolute_sample_index = 0;

// static buffers - no malloc inside audio task
static uint32_t i2s_word_slots[FRAMES_PER_PACKET * 2]; // L0,R0,L1,R1...
static uint32_t payload_words[FRAMES_PER_PACKET];

Preferences prefs;

// ---------------- utility validators ----------------
bool looksLikeIP(const char* s) {
  // quick validation: A.B.C.D where each 0..255
  int a,b,c,d;
  char extra;
  if (sscanf(s, "%d.%d.%d.%d%c", &a,&b,&c,&d,&extra) == 4) {
    if (a>=0 && a<=255 && b>=0 && b<=255 && c>=0 && c<=255 && d>=0 && d<=255) return true;
  }
  return false;
}

bool looksLikePort(const char* s) {
  int p = atoi(s);
  return (p > 0 && p <= 65535);
}

// ---------- wifi manager ----------
void wifiConnectmanager() {
  WiFi.mode(WIFI_STA);

  prefs.begin("config", false);
  String saved_ip = prefs.getString("pc_ip", "");
  String saved_port = prefs.getString("pc_port", "");

  // Pre-fill global vars if saved
  if (saved_ip.length() > 0) PC_IP = saved_ip;
  if (saved_port.length() > 0) PC_PORT = (uint16_t) saved_port.toInt();

  // provide initial values to portal fields
  char ip_buf[40]; saved_ip.toCharArray(ip_buf, sizeof(ip_buf));
  char port_buf[8]; saved_port.toCharArray(port_buf, sizeof(port_buf));

  WiFiManager wm;
  WiFiManagerParameter custom_ip("pcip", "Receiver IP (e.g. 192.168.2.133)", ip_buf, 40);
  WiFiManagerParameter custom_port("pcport", "Receiver Port (e.g. 7000)", port_buf, 6);

  wm.addParameter(&custom_ip);
  wm.addParameter(&custom_port);

  Serial.println("[WIFI] starting AutoConnect (WiFiManager)...");
  bool res = wm.autoConnect(WIFI_AP_NAME, WIFI_AP_PASS);

  if (!res) {
    Serial.println("WiFiManager failed to connect or user cancelled.");
    // even if portal cancelled, we keep any existing saved values; else there's nothing we can do
  } else {
    Serial.println("WiFi connected via WiFiManager.");
    // read values the user entered (or left as pre-fill)
    const char* new_ip = custom_ip.getValue();
    const char* new_port = custom_port.getValue();

    // basic validation and save
    if (strlen(new_ip) > 0 && looksLikeIP(new_ip)) {
      PC_IP = String(new_ip);
      prefs.putString("pc_ip", PC_IP);
      Serial.printf("[WIFI] saved PC_IP=%s\n", PC_IP.c_str());
    } else {
      Serial.println("[WIFI] warning: provided IP looks invalid or empty. Not saved.");
    }

    if (strlen(new_port) > 0 && looksLikePort(new_port)) {
      PC_PORT = (uint16_t)atoi(new_port);
      prefs.putString("pc_port", String(PC_PORT));
      Serial.printf("[WIFI] saved PC_PORT=%u\n", (unsigned)PC_PORT);
    } else {
      Serial.println("[WIFI] warning: provided port looks invalid or empty. Not saved.");
    }
  }

  prefs.end();

  // show what we will use (if any)
  if (PC_IP.length()) Serial.printf("[WIFI] using PC_IP=%s\n", PC_IP.c_str());
  else Serial.println("[WIFI] no PC_IP configured yet.");

  if (PC_PORT) Serial.printf("[WIFI] using PC_PORT=%u\n", (unsigned)PC_PORT);
  else Serial.println("[WIFI] no PC_PORT configured yet.");
}

// ---------- i2s init ----------
void i2s_init() {
  i2s_config_t i2s_config = {
    .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX),
    .sample_rate = SAMPLE_RATE,
    .bits_per_sample = I2S_BITS,
    .channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT,
    .communication_format = (i2s_comm_format_t)(I2S_COMM_FORMAT_I2S | I2S_COMM_FORMAT_I2S_MSB),
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

  esp_err_t err = i2s_driver_install(I2S_NUM_0, &i2s_config, 0, NULL);
  if (err != ESP_OK) {
    Serial.printf("[I2S] driver_install failed: %d\n", err);
    while (1) delay(500);
  }
  err = i2s_set_pin(I2S_NUM_0, &pin_config);
  if (err != ESP_OK) {
    Serial.printf("[I2S] set_pin failed: %d\n", err);
    while (1) delay(500);
  }
  i2s_zero_dma_buffer(I2S_NUM_0);
  Serial.println("[I2S] initialized (APLL enabled)");
}

// ---------- header packing (matches spec) ----------
void write_tcp_header(uint32_t seq,
                      uint64_t first_sample_index,
                      uint64_t timestamp_us,
                      uint16_t number_of_frames)
{
  // magic [0..3]
  header_buf[0] = (uint8_t)(HEADER_MAGIC & 0xFF);
  header_buf[1] = (uint8_t)((HEADER_MAGIC >> 8) & 0xFF);
  header_buf[2] = (uint8_t)((HEADER_MAGIC >> 16) & 0xFF);
  header_buf[3] = (uint8_t)((HEADER_MAGIC >> 24) & 0xFF);

  // seq [4..7]
  for (int i = 0; i < 4; ++i) header_buf[4 + i] = (uint8_t)((seq >> (8 * i)) & 0xFF);

  // first_sample_index [8..15]
  for (int i = 0; i < 8; ++i) header_buf[8 + i] = (uint8_t)((first_sample_index >> (8 * i)) & 0xFF);

  // timestamp_us [16..23]
  for (int i = 0; i < 8; ++i) header_buf[16 + i] = (uint8_t)((timestamp_us >> (8 * i)) & 0xFF);

  // frames [24..25]
  header_buf[24] = (uint8_t)(number_of_frames & 0xFF);
  header_buf[25] = (uint8_t)((number_of_frames >> 8) & 0xFF);

  // channels [26]
  header_buf[26] = (uint8_t)NUMBER_CHANNELS;

  // bytes_per_sample [27]
  header_buf[27] = (uint8_t)BYTES_PER_SAMPLE;

  // sample_rate [28..31]
  header_buf[28] = (uint8_t)(SAMPLE_RATE & 0xFF);
  header_buf[29] = (uint8_t)((SAMPLE_RATE >> 8) & 0xFF);
  header_buf[30] = (uint8_t)((SAMPLE_RATE >> 16) & 0xFF);
  header_buf[31] = (uint8_t)((SAMPLE_RATE >> 24) & 0xFF);

  // format_id [32..33]
  header_buf[32] = (uint8_t)(FORMAT_INT32_LEFT24 & 0xFF);
  header_buf[33] = (uint8_t)((FORMAT_INT32_LEFT24 >> 8) & 0xFF);
}

// ---------- send packet (header + payload) ----------
bool send_tcp_packet(WiFiClient &client, const uint32_t* payload_words_pointer, size_t payload_bytes)
{
  if (!client || !client.connected()) return false;

  // write header
  size_t hsent = 0;
  while (hsent < (size_t)HEADER_SIZE) {
    int w = client.write(header_buf + hsent, HEADER_SIZE - hsent);
    if (w <= 0) return false;
    hsent += (size_t)w;
  }

  // write payload (payload_bytes)
  const uint8_t* p = (const uint8_t*)payload_words_pointer;
  size_t sent = 0;
  while (sent < payload_bytes) {
    int w = client.write(p + sent, payload_bytes - sent);
    if (w <= 0) return false;
    sent += (size_t)w;
    if ((sent % 1024) == 0) taskYIELD();
  }

  return true;
}

// ---------- audio task ----------
void audioTask(void *pv)
{
  (void)pv;
  Serial.printf("[TASK] starting audioTask: FRAMES=%u bytesToRead=%u payload=%u\n",
                FRAMES_PER_PACKET, (unsigned)BYTES_TO_READ, (unsigned)PAYLOAD_BYTES);

  while (true) {
    // ensure TCP connection only if we have a configured target
    if (PC_IP.length() == 0 || PC_PORT == 0) {
      // no remote configured: wait & yield so user can configure via portal on next boot
      vTaskDelay(pdMS_TO_TICKS(1000));
      continue;
    }

    if (!TCPCLIENT || !TCPCLIENT.connected()) {
      Serial.printf("[TCP] connecting to %s:%u ...\n", PC_IP.c_str(), (unsigned)PC_PORT);
      if (TCPCLIENT.connect(PC_IP.c_str(), PC_PORT)) {
        TCPCLIENT.setNoDelay(true);
        Serial.println("[TCP] connected");
      } else {
        Serial.println("[TCP] connect failed, retry in 1s");
        delay(1000);
        continue;
      }
    }

    // read from I2S (blocking)
    size_t bytes_read = 0;
    esp_err_t res = i2s_read(I2S_NUM_0, (void*)i2s_word_slots, BYTES_TO_READ, &bytes_read, portMAX_DELAY);
    if (res != ESP_OK || bytes_read == 0) {
      Serial.printf("[I2S] read err %d bytes=%u\n", res, (unsigned)bytes_read);
      delay(10);
      continue;
    }

    size_t word_count = bytes_read / 4;
    size_t available_frames = 0;
    if (word_count >= NEEDED_WORDS) {
      for (size_t i = 0; i < FRAMES_PER_PACKET; ++i) payload_words[i] = i2s_word_slots[i * 2 + 1];
      available_frames = FRAMES_PER_PACKET;
    } else {
      size_t maxFrames = word_count / 2;
      for (size_t i = 0; i < maxFrames; ++i) payload_words[i] = i2s_word_slots[i * 2 + 1];
      for (size_t i = maxFrames; i < FRAMES_PER_PACKET; ++i) payload_words[i] = 0;
      available_frames = maxFrames;
    }

    uint32_t seq = ++sequence_counter;
    uint64_t first_index = absolute_sample_index;
    uint64_t ts = (uint64_t)esp_timer_get_time();

    write_tcp_header(seq, first_index, ts, (uint16_t)available_frames);

    if (!send_tcp_packet(TCPCLIENT, payload_words, available_frames * BYTES_PER_SAMPLE)) {
      Serial.println("[TCP] send failed, reconnecting");
      TCPCLIENT.stop();
      delay(50);
      continue;
    }

    absolute_sample_index += (uint64_t)available_frames;
    taskYIELD();
  }

  vTaskDelete(NULL);
}

void startAudioTask() {
  xTaskCreatePinnedToCore(audioTask, "audioTask", 8192, NULL, 6, NULL, 1);
}

// ---------- setup / loop ----------
void setup() {
  Serial.begin(115200);
  while (!Serial) delay(5);
  Serial.println("\n=== corrected ESP32 I2S -> TCP streamer (WiFiManager) ===");

  wifiConnectmanager();
  i2s_init();
  startAudioTask();
  Serial.println("setup done");
}

void loop() {
  static unsigned long last = 0;
  if (millis() - last > 2000) {
    last = millis();
    Serial.printf("[STAT] WiFi=%s TCP=%s seq=%u sample_idx=%llu PC=%s:%u\n",
                  WiFi.isConnected() ? "OK" : "NO",
                  TCPCLIENT.connected() ? "OK" : "NO",
                  (unsigned)sequence_counter,
                  (unsigned long long)absolute_sample_index,
                  PC_IP.length() ? PC_IP.c_str() : "none",
                  (unsigned)PC_PORT);
  }
  delay(100);
}
