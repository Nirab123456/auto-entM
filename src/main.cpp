// esp32_stream_tcp_i2s_fixed_high_quality.ino
// Reliable TCP streamer for I2S microphone (INMP441) -> PC
// Improvements: microsecond timestamps, 64-bit sample index, static buffers,
// APLL for stable sample clock, better header, no malloc in audio loop.

#include <Arduino.h>
#include <WiFi.h>
#include "driver/i2s.h"

// ---------- CONFIG (edit for your network / receiver) ----------
const char* WIFI_SSID = "94 Pembroke Street - 2"; // <- REPLACE
const char* WIFI_PASS = "welcomehome";           // <- REPLACE
const char* PC_IP      = "192.168.2.142";         // <- REPLACE with your PC IP
const uint16_t PC_PORT = 7000;                   // <- PC listening TCP port
// ----------------------------------------------------------------

// I2S pins (set to your wiring)
const int PIN_BCK  = 7;   // BCLK
const int PIN_WS   = 15;  // LRCLK
const int PIN_DATA = 16;  // SD

// Capture parameters (tweak for quality vs bandwidth)
const int SAMPLE_RATE = 48000;                       // 48 kHz for media
const i2s_bits_per_sample_t I2S_BITS = I2S_BITS_PER_SAMPLE_32BIT; // 32-bit slots (24-bit left-aligned audio)
const int CHANNELS = 1;                              // mono microphone (mic in right slot)
const int FRAMES_PER_PACKET = 1024;                  // samples per packet (per channel)
const int BYTES_PER_SAMPLE = 4;                      // 32-bit words

// Header: new self-describing layout (little-endian)
 // layout (bytes):
 // [0..3]   : uint32_t magic (ASCII tag)
 // [4..7]   : uint32_t seq
 // [8..15]  : uint64_t first_sample_index  (index of first sample in payload)
 // [16..23] : uint64_t timestamp_us        (esp_timer_get_time() for first sample)
 // [24..25] : uint16_t frames
 // [26]     : uint8_t  channels
 // [27]     : uint8_t  bytes_per_sample
 // [28..31] : uint32_t sample_rate
 // [32..33] : uint16_t format_id
const int HEADER_SIZE = 34;
const uint32_t HEADER_MAGIC = 0x45535032; // 'E' 'S' 'P' '2'
const uint16_t FORMAT_INT32_LEFT24 = 1;   // format id: int32 left-aligned 24-bit

// Global WiFi client
WiFiClient tcpClient;

// Sequence counter and sample index
volatile uint32_t seq_counter = 0;
uint64_t global_sample_index = 0; // absolute sample index (increments by frames sent)

// Static buffers (allocated in bss, no malloc inside audioTask)
static uint32_t i2s_words[FRAMES_PER_PACKET * 2];   // stereo slots: L0,R0,L1,R1...
static uint32_t payload_words[FRAMES_PER_PACKET];   // right-channel words to send

// Helper: connect WiFi (blocking, with simple retry)
void wifiConnect() {
  Serial.printf("[WIFI] connecting to '%s' ...", WIFI_SSID);
  WiFi.mode(WIFI_MODE_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED) {
    Serial.print('.');
    delay(300);
    if (millis() - start > 20000) {
      Serial.println("\n[WIFI] connect timeout, retrying...");
      start = millis();
      WiFi.disconnect();
      WiFi.begin(WIFI_SSID, WIFI_PASS);
    }
  }
  Serial.println();
  Serial.print("[WIFI] connected, IP: "); Serial.println(WiFi.localIP());
}

// Initialize I2S for INMP441 (right-channel mono mic)
void i2sInit() {
  i2s_config_t i2s_config = {
    .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX),
    .sample_rate = SAMPLE_RATE,
    .bits_per_sample = I2S_BITS,
    .channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT, // read stereo slots; mic uses RIGHT
    .communication_format = (i2s_comm_format_t)(I2S_COMM_FORMAT_I2S | I2S_COMM_FORMAT_I2S_MSB),
    .intr_alloc_flags = 0,
    .dma_buf_count = 6,        // several DMA buffers (robustness)
    .dma_buf_len = FRAMES_PER_PACKET / 2, // buffer length in samples (tune)
    .use_apll = true,          // use APLL for more accurate sampling clock
    .tx_desc_auto_clear = false,
    .fixed_mclk = 0
  };

  i2s_pin_config_t pin_config = {
    .bck_io_num = PIN_BCK,
    .ws_io_num = PIN_WS,
    .data_out_num = I2S_PIN_NO_CHANGE,
    .data_in_num = PIN_DATA
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

// Build and send one framed packet over TCP (blocking writes, loop handles partial writes)
bool sendPacketTCP(WiFiClient &client,
                   uint32_t seq,
                   uint64_t first_sample_index,
                   uint64_t timestamp_us,
                   const uint32_t* payload_words_ptr,
                   uint16_t frames) {
  if (!client || !client.connected()) return false;

  // Build header (little-endian)
  uint8_t header[HEADER_SIZE];
  // magic
  header[0] = (uint8_t)(HEADER_MAGIC & 0xFF);
  header[1] = (uint8_t)((HEADER_MAGIC >> 8) & 0xFF);
  header[2] = (uint8_t)((HEADER_MAGIC >> 16) & 0xFF);
  header[3] = (uint8_t)((HEADER_MAGIC >> 24) & 0xFF);
  // seq
  header[4] = (uint8_t)(seq & 0xFF);
  header[5] = (uint8_t)((seq >> 8) & 0xFF);
  header[6] = (uint8_t)((seq >> 16) & 0xFF);
  header[7] = (uint8_t)((seq >> 24) & 0xFF);
  // first_sample_index (8 bytes)
  for (int i = 0; i < 8; ++i) header[8 + i] = (uint8_t)((first_sample_index >> (8*i)) & 0xFF);
  // timestamp_us (8 bytes)
  for (int i = 0; i < 8; ++i) header[16 + i] = (uint8_t)((timestamp_us >> (8*i)) & 0xFF);
  // frames (uint16)
  header[24] = (uint8_t)(frames & 0xFF);
  header[25] = (uint8_t)((frames >> 8) & 0xFF);
  // channels (1 byte)
  header[26] = (uint8_t)CHANNELS;
  // bytes_per_sample (1 byte)
  header[27] = (uint8_t)BYTES_PER_SAMPLE;
  // sample_rate (uint32)
  header[28] = (uint8_t)(SAMPLE_RATE & 0xFF);
  header[29] = (uint8_t)((SAMPLE_RATE >> 8) & 0xFF);
  header[30] = (uint8_t)((SAMPLE_RATE >> 16) & 0xFF);
  header[31] = (uint8_t)((SAMPLE_RATE >> 24) & 0xFF);
  // format_id (uint16)
  header[32] = (uint8_t)(FORMAT_INT32_LEFT24 & 0xFF);
  header[33] = (uint8_t)((FORMAT_INT32_LEFT24 >> 8) & 0xFF);

  // write header
  size_t hsent = 0;
  while (hsent < HEADER_SIZE) {
    int w = client.write(header + hsent, HEADER_SIZE - hsent);
    if (w <= 0) return false;
    hsent += (size_t)w;
  }

  // payload: write frames * BYTES_PER_SAMPLE bytes
  size_t payload_bytes = (size_t)frames * CHANNELS * BYTES_PER_SAMPLE;
  const uint8_t* p = (const uint8_t*)payload_words_ptr; // payload_words_ptr is an array of uint32
  size_t sent = 0;
  while (sent < payload_bytes) {
    int w = client.write(p + sent, payload_bytes - sent);
    if (w <= 0) return false;
    sent += (size_t)w;
    // yield occasionally to keep RTOS happy if sending large payloads
    if ((sent % 1024) == 0) taskYIELD();
  }

  // do not call client.flush() here; flushing may block indefinitely under congestion.
  return true;
}

// audioTask: read I2S frames and send via TCP
void audioTask(void *pv) {
  (void)pv;

  // Precomputed sizes
  const size_t bytesToRead = (size_t)FRAMES_PER_PACKET * BYTES_PER_SAMPLE * 2; // stereo slots in bytes
  const size_t payload_bytes = (size_t)FRAMES_PER_PACKET * BYTES_PER_SAMPLE * CHANNELS;

  Serial.printf("[TASK] starting audioTask: FRAMES=%u bytesToRead=%u payload=%u\n",
                FRAMES_PER_PACKET, (unsigned)bytesToRead, (unsigned)payload_bytes);

  while (true) {
    // Ensure TCP connection
    if (!tcpClient || !tcpClient.connected()) {
      Serial.println("[TCP] connecting to server...");
      if (tcpClient.connect(PC_IP, PC_PORT)) {
        tcpClient.setNoDelay(true); // disable Nagle to reduce latency
        Serial.println("[TCP] connected");
        // On new session, you might want to reset indices or handshake; here we continue indices.
      } else {
        Serial.println("[TCP] connect failed, retry in 1s");
        delay(1000);
        continue;
      }
    }

    // Block until i2s buffer filled for requested bytes
    size_t bytes_read = 0;
    esp_err_t res = i2s_read(I2S_NUM_0, (void*)i2s_words, bytesToRead, &bytes_read, portMAX_DELAY);
    if (res != ESP_OK || bytes_read == 0) {
      Serial.printf("[I2S] read err %d bytes=%u\n", res, (unsigned)bytes_read);
      // brief delay to avoid tight loop on persistent error
      delay(10);
      continue;
    }

    // words contains 32-bit words L0,R0,L1,R1...
    size_t word_count = bytes_read / 4;
    size_t need_words = (size_t)FRAMES_PER_PACKET * 2;
    size_t avail_frames = 0;
    if (word_count >= need_words) {
      // Extract right channel words for FRAMES_PER_PACKET frames
      for (size_t i = 0; i < FRAMES_PER_PACKET; ++i) {
        payload_words[i] = i2s_words[i*2 + 1];
      }
      avail_frames = FRAMES_PER_PACKET;
    } else {
      // Partial read: extract what we can and zero-fill the remainder
      size_t maxFrames = word_count / 2;
      for (size_t i = 0; i < maxFrames; ++i) payload_words[i] = i2s_words[i*2 + 1];
      for (size_t i = maxFrames; i < FRAMES_PER_PACKET; ++i) payload_words[i] = 0;
      avail_frames = maxFrames;
    }

    // Prepare header info
    uint32_t seq = ++seq_counter;
    uint64_t first_sample_index = global_sample_index;
    uint64_t ts_us = (uint64_t)esp_timer_get_time(); // microseconds since boot
    // send packet
    bool ok = sendPacketTCP(tcpClient, seq, first_sample_index, ts_us, payload_words, (uint16_t)avail_frames);
    if (!ok) {
      Serial.println("[TCP] send failed, will reconnect");
      tcpClient.stop();
      delay(50);
      continue;
    }

    // advance global sample index by frames actually packaged
    global_sample_index += (uint64_t)avail_frames;

    // keep audio task tight for low latency
    taskYIELD();
  }

  // never reached
  vTaskDelete(NULL);
}

void startAudioTask() {
  // larger stack because we use buffers and networking inside task
  xTaskCreatePinnedToCore(audioTask, "audioTask", 8192, NULL, 6, NULL, 1);
}

void setup() {
  Serial.begin(115200);
  while (!Serial) delay(5);
  Serial.println("\n=== ESP32 I2S -> TCP streamer (high-quality, sample-indexed) ===");

  wifiConnect();
  i2sInit();

  // Start audio task
  startAudioTask();

  Serial.println("[SETUP] done");
}

void loop() {
  // Main loop prints status occasionally
  static unsigned long last = 0;
  if (millis() - last > 2000) {
    last = millis();
    Serial.printf("[STAT] WiFi=%s TCP=%s seq=%u sample_idx=%llu\n",
                  WiFi.isConnected() ? "OK" : "NO",
                  tcpClient.connected() ? "OK" : "NO",
                  (unsigned)seq_counter,
                  (unsigned long long)global_sample_index);
  }
  delay(100);
}
