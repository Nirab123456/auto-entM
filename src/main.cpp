// corrected_esp32_stream_tcp_i2s.ino
// Fixed & cleaned up version of your TCP streamer sketch (I2S -> TCP).
// Uses WiFiManager + Preferences to optionally capture receiver IP/port.

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiManager.h>   // Install via Library Manager
#include "driver/i2s.h"
#include "esp_timer.h"     // esp_timer_get_time()
#include <Preferences.h>

// ---------- user configuration (edit if desired) ----------
const char* WIFI_SSID = "94 Pembroke Street - 2";
const char* WIFI_PASS = "welcomehome";

String PC_IP      = "";   // populated from prefs or WiFiManager field
uint16_t PC_PORT  = 7000; // populated from prefs or WiFiManager field

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
const size_t BYTES_TO_READ = (size_t)FRAMES_PER_PACKET * BYTES_PER_SAMPLE * 2; // stereo slots (bytes)
const size_t PAYLOAD_BYTES  = (size_t)FRAMES_PER_PACKET * BYTES_PER_SAMPLE * NUMBER_CHANNELS;
const size_t NEEDED_WORDS   = (size_t)FRAMES_PER_PACKET * 2; // words (32-bit) needed from i2s_words

// header constants (must match receiver)
constexpr int HEADER_SIZE = 34;
const uint32_t HEADER_MAGIC = 0x45535032; // 'ESP2'
const uint16_t FORMAT_INT32_LEFT24 = 1;

// globals
static uint8_t header_buf[HEADER_SIZE];
WiFiClient TCPCLIENT;

// atomics/indices
volatile uint32_t sequence_counter = 0;
uint64_t absolute_sample_index = 0;

// static buffers (no malloc inside audio task)
static uint32_t i2s_word_slots[FRAMES_PER_PACKET * 2]; // L0,R0,L1,R1...
static uint32_t payload_words[FRAMES_PER_PACKET];

// Preferences + WiFiManager
Preferences prefs;
WiFiManager wm;

// -------------------- helpers for prefs & WiFiManager --------------------

static bool looks_like_ip(const char* s) {
  int a, b, c, d;
  char tail;
  // sscanf returns number of items successfully parsed. We expect exactly 4 and no trailing junk.
  int n = sscanf(s, "%d.%d.%d.%d%c", &a, &b, &c, &d, &tail);
  if (n == 4) {
    return (a >= 0 && a <= 255 && b >= 0 && b <= 255 && c >= 0 && c <= 255 && d >= 0 && d <= 255);
  }
  return false;
}

static bool looks_like_port(const char* s) {
  long p = atol(s);
  return (p > 0 && p <= 65535);
}

// Load saved PC IP/port from Preferences into PC_IP and PC_PORT
void load_prefs() {
  prefs.begin("config", true); // read-only mode
  String saved_ip = prefs.getString("pc_ip", "");
  String saved_port = prefs.getString("pc_port", "");
  if (saved_ip.length() > 0 && looks_like_ip(saved_ip.c_str())) PC_IP = saved_ip;
  if (saved_port.length() > 0 && looks_like_port(saved_port.c_str())) PC_PORT = (uint16_t)saved_port.toInt();
  prefs.end();
}

// Save PC IP/port into Preferences
void save_prefs(const char* ip, const char* port) {
  prefs.begin("config", false);
  prefs.putString("pc_ip", String(ip));
  prefs.putString("pc_port", String(port));
  prefs.end();
}

// Present WiFiManager portal with extra fields for PC IP/port, load/save using Preferences
void setup_wifi_and_params() {
  // Load saved values if present
  load_prefs();

  // Prepare default buffers for the portal fields
  char ipbuf[40]; ipbuf[0] = 0;
  char portbuf[16]; portbuf[0] = 0;
  if (PC_IP.length()) PC_IP.toCharArray(ipbuf, sizeof(ipbuf));
  if (PC_PORT) snprintf(portbuf, sizeof(portbuf), "%u", (unsigned)PC_PORT);

  // Create parameter fields
  WiFiManagerParameter ip_param("pcip", "Receiver IP (e.g. 192.168.2.133)", ipbuf, sizeof(ipbuf));
  WiFiManagerParameter port_param("pcport", "Receiver Port (e.g. 7000)", portbuf, sizeof(portbuf));

  wm.addParameter(&ip_param);
  wm.addParameter(&port_param);

  Serial.println("[WIFI] starting WiFiManager autoConnect (opens AP if needed)...");
  bool res = wm.autoConnect(WIFI_AP_NAME, WIFI_AP_PASS); // blocks until user configures or auto-joins
  if (!res) {
    Serial.println("[WIFI] WiFiManager failed to connect or user cancelled.");
    // Continue: device will try to use any existing remembered WiFi credentials
  } else {
    Serial.println("[WIFI] Connected to WiFi via WiFiManager.");
    // read values entered in portal
    const char* ipv = ip_param.getValue();
    const char* portv = port_param.getValue();
    if (ipv && ipv[0] && looks_like_ip(ipv)) {
      PC_IP = String(ipv);
    }
    if (portv && portv[0] && looks_like_port(portv)) {
      PC_PORT = (uint16_t)atoi(portv);
    }
    // Save back to prefs
    save_prefs(PC_IP.c_str(), String(PC_PORT).c_str());
    Serial.printf("[WIFI] saved PC IP=%s port=%u\n", PC_IP.c_str(), (unsigned)PC_PORT);
  }

  // remove parameters so we can recreate later if needed (WiFiManager retains them otherwise)
  wm.resetSettings();
}

// ---------- I2S init ----------
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
  if (!client.connected()) return false;

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
    // ensure TCP connection
    if (!TCPCLIENT.connected()) {
      if (!PC_IP.length() || !PC_PORT) {
        // No receiver configured yet; yield to allow main loop to handle config
        vTaskDelay(pdMS_TO_TICKS(2000));
        continue;
      }
      Serial.println("[TCP] connecting...");
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
  Serial.println("\n=== corrected ESP32 I2S -> TCP streamer ===");

  // Setup WiFi and allow user to enter receiver IP/port via captive portal
  setup_wifi_and_params();

  // initialize I2S peripheral
  i2s_init();

  // start audio RT task
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
                  PC_IP.length() ? PC_IP.c_str() : "(none)",
                  (unsigned)PC_PORT);
  }
  delay(100);
}
