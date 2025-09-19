// corrected_esp32_stream_tcp_i2s_v13_large_ring_clear_on_disconnect.ino
// v13: larger ring (128), clear-buffers-on-disconnect, start capture only after handshake

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiManager.h>
#include "driver/i2s.h"
#include "esp_timer.h"
#include <Preferences.h>
#include <atomic>
#include <mutex>

// ---------- CONFIG ----------
const uint8_t BUTTON_PIN = 4; // reset button (active LOW)
const uint16_t BUTTON_HOLD_MS = 800;

const char* WIFI_AP_NAME = "AutoConnectAP";
const char* WIFI_AP_PASS = "password";

// I2S / audio params
const uint8_t PIN_CLK = 7;
const uint8_t PIN_WS  = 15;
const uint8_t PIN_SD  = 16;
const uint32_t SAMPLE_RATE = 48000;
const i2s_bits_per_sample_t I2S_BITS = I2S_BITS_PER_SAMPLE_32BIT;
const uint8_t NUMBER_CHANNELS = 1;
const uint16_t FRAMES_PER_PACKET = 1024;
const uint8_t BYTES_PER_SAMPLE = 4;

// derived sizes
const size_t BYTES_TO_READ = (size_t)FRAMES_PER_PACKET * BYTES_PER_SAMPLE * 2;
const size_t PAYLOAD_BYTES  = (size_t)FRAMES_PER_PACKET * BYTES_PER_SAMPLE * NUMBER_CHANNELS;
const size_t NEEDED_WORDS   = (size_t)FRAMES_PER_PACKET * 2;

// header constants
constexpr int HEADER_SIZE = 34;
const uint32_t HEADER_MAGIC = 0x45535032;
const uint16_t FORMAT_INT32_LEFT24 = 1;

// RING: increased to 128 to use more RAM for buffering
// 128 * 1024 frames * 4 bytes ≈ 512 KB
constexpr size_t RING_SIZE = 64;   // power of two
constexpr size_t RING_MASK = RING_SIZE - 1;

// ---------- global buffers / atomics ----------
static uint32_t i2s_word_slots[FRAMES_PER_PACKET * 2];
static uint32_t payload_words[FRAMES_PER_PACKET];

static uint32_t ring_payload[RING_SIZE][FRAMES_PER_PACKET];
static uint16_t ring_frames[RING_SIZE];
static uint64_t ring_first_index[RING_SIZE];
static uint64_t ring_timestamp[RING_SIZE];

std::atomic<size_t> ring_head{0};
std::atomic<size_t> ring_tail{0};

WiFiClient TCPCLIENT;

std::atomic<uint32_t> sequence_counter{0};
std::atomic<uint64_t> absolute_sample_index{0};

static uint8_t header_buf[HEADER_SIZE];

std::atomic<bool> consumer_ready{false};

TaskHandle_t audioTaskHandle = NULL;
TaskHandle_t networkTaskHandle = NULL;

// Preferences namespace
static const char* PREF_NAMESPACE = "config";

// Forward declarations
void startTasks();
void stopTasks();
void i2s_init();
void write_tcp_header(uint32_t seq, uint64_t first_sample_index, uint64_t timestamp_us, uint16_t number_of_frames);
void setup_wifi_and_params();
void startConfigPortalFromButton();
void audioTask(void *pv);
void networkTask(void *pv);

// ---------- ReceiverConfig ----------
class ReceiverConfig {
private:
  Preferences prefs_;
  std::mutex mu_;
  IPAddress ip_;
  uint16_t port_ = 0;
public:
  ReceiverConfig() {}
  ~ReceiverConfig() { prefs_.end(); }

  void begin() { prefs_.begin(PREF_NAMESPACE, true); load(); prefs_.end(); }
  void load() {
    std::lock_guard<std::mutex> lock(mu_);
    prefs_.begin(PREF_NAMESPACE, true);
    String saved_ip = prefs_.getString("pc_ip", "");
    String saved_port = prefs_.getString("pc_port", "");
    prefs_.end();
    IPAddress tmp;
    if (saved_ip.length() && tmp.fromString(saved_ip)) ip_ = tmp; else ip_ = IPAddress(0,0,0,0);
    if (saved_port.length()) {
      long p = saved_port.toInt();
      port_ = (p>0 && p<=65535) ? (uint16_t)p : 0;
    } else port_ = 0;
  }
  void save(const char* ipstr, uint16_t port) {
    std::lock_guard<std::mutex> lock(mu_);
    prefs_.begin(PREF_NAMESPACE, false);
    prefs_.putString("pc_ip", String(ipstr));
    prefs_.putString("pc_port", String((unsigned)port));
    prefs_.end();
    IPAddress tmp;
    if (ipstr && ipstr[0] && tmp.fromString(String(ipstr))) { ip_ = tmp; port_ = port; }
    else { ip_ = IPAddress(0,0,0,0); port_ = 0; }
  }
  void clear() {
    std::lock_guard<std::mutex> lock(mu_);
    prefs_.begin(PREF_NAMESPACE, false);
    prefs_.remove("pc_ip"); prefs_.remove("pc_port");
    prefs_.end();
    ip_ = IPAddress(0,0,0,0); port_ = 0;
  }
  void get(IPAddress &out_ip, uint16_t &out_port) { std::lock_guard<std::mutex> lock(mu_); out_ip = ip_; out_port = port_; }
  String ipString() { std::lock_guard<std::mutex> lock(mu_); return ip_ ? String(ip_.toString()) : String("(none)"); }
  unsigned short port() { std::lock_guard<std::mutex> lock(mu_); return port_; }
  bool isValid() { std::lock_guard<std::mutex> lock(mu_); return (ip_ != IPAddress(0,0,0,0) && port_ != 0); }
};

static ReceiverConfig gConfig;

// ---------- ring helpers ----------
void clearRingAndResetIndices() {
  // caller must ensure consumer_ready == false before calling
  ring_head.store(0, std::memory_order_relaxed);
  ring_tail.store(0, std::memory_order_relaxed);
  for (size_t i=0;i<RING_SIZE;++i) {
    ring_frames[i] = 0;
    ring_first_index[i] = 0;
    ring_timestamp[i] = 0;
    // we do not need to zero payload memory (costly) — just metadata is sufficient
  }
  sequence_counter.store(0, std::memory_order_relaxed);
  absolute_sample_index.store(0, std::memory_order_relaxed);
  Serial.printf("[RING] cleared metadata and reset sequence/sample indices (RING_SIZE=%u)\n", (unsigned)RING_SIZE);
}

// ---------- WiFi / WiFiManager portal ----------
void setup_wifi_and_params() {
  gConfig.load();
  String ipDefault = gConfig.ipString();
  char ipbuf[40]; ipbuf[0]=0; ipDefault.toCharArray(ipbuf, sizeof(ipbuf));
  unsigned short pd = gConfig.port();
  char portbuf[16]; portbuf[0]=0; if (pd) snprintf(portbuf, sizeof(portbuf), "%u", (unsigned)pd);

  WiFiManager wm_local;
  WiFiManagerParameter ip_param("pcip", "Receiver IP (e.g. 192.168.2.133)", ipbuf, sizeof(ipbuf));
  WiFiManagerParameter port_param("pcport", "Receiver Port (e.g. 7000)", portbuf, sizeof(portbuf));
  wm_local.addParameter(&ip_param);
  wm_local.addParameter(&port_param);

  Serial.println("[WIFI] launching WiFiManager.autoConnect() (portal will show IP/port fields)");
  bool res = wm_local.autoConnect(WIFI_AP_NAME, WIFI_AP_PASS);
  if (!res) {
    Serial.println("[WIFI] WiFiManager autoConnect failed or canceled");
    consumer_ready.store(false, std::memory_order_release);
    return;
  }

  const char* entered_ip = ip_param.getValue();
  const char* entered_port = port_param.getValue();
  if (entered_ip && entered_ip[0]) {
    IPAddress tmp;
    if (tmp.fromString(String(entered_ip))) {
      unsigned p = 0;
      if (entered_port && entered_port[0]) p = (unsigned)atoi(entered_port);
      if (p>0 && p<=65535) {
        gConfig.save(entered_ip, (uint16_t)p);
        Serial.printf("[WIFI] saved PC IP=%s PORT=%u\n", entered_ip, (unsigned)p);
      } else {
        gConfig.save(entered_ip, 0);
        Serial.println("[WIFI] saved PC IP (no valid port)");
      }
    } else {
      Serial.println("[WIFI] invalid IP entered, not saved");
      gConfig.clear();
    }
  } else {
    Serial.println("[WIFI] no PC IP entered");
  }
  consumer_ready.store(false, std::memory_order_release);
}

void startConfigPortalFromButton() {
  Serial.println("[RESET] button hold: clearing saved prefs and opening portal...");
  stopTasks();
  gConfig.clear();
  WiFiManager wm_temp; wm_temp.resetSettings();
  WiFi.disconnect(true, true); WiFi.mode(WIFI_STA);
  delay(200);
  setup_wifi_and_params();
  // clear ring metadata and reset indices (safe while tasks stopped)
  clearRingAndResetIndices();
  startTasks();
  Serial.println("[RESET] portal finished, tasks restarted");
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
  i2s_pin_config_t pin_config = { .bck_io_num = PIN_CLK, .ws_io_num = PIN_WS, .data_out_num = I2S_PIN_NO_CHANGE, .data_in_num = PIN_SD };
  esp_err_t err = i2s_driver_install(I2S_NUM_0, &i2s_config, 0, NULL);
  if (err != ESP_OK) { Serial.printf("[I2S] driver_install failed: %d\n", err); while(1) delay(500); }
  err = i2s_set_pin(I2S_NUM_0, &pin_config);
  if (err != ESP_OK) { Serial.printf("[I2S] set_pin failed: %d\n", err); while(1) delay(500); }
  i2s_zero_dma_buffer(I2S_NUM_0);
  Serial.println("[I2S] initialized (APLL enabled)");
}

// ---------- header packing ----------
void write_tcp_header(uint32_t seq, uint64_t first_sample_index, uint64_t timestamp_us, uint16_t number_of_frames) {
  header_buf[0] = (uint8_t)(HEADER_MAGIC & 0xFF);
  header_buf[1] = (uint8_t)((HEADER_MAGIC >> 8) & 0xFF);
  header_buf[2] = (uint8_t)((HEADER_MAGIC >> 16) & 0xFF);
  header_buf[3] = (uint8_t)((HEADER_MAGIC >> 24) & 0xFF);
  for (int i=0;i<4;++i) header_buf[4+i] = (uint8_t)((seq >> (8*i)) & 0xFF);
  for (int i=0;i<8;++i) header_buf[8+i] = (uint8_t)((first_sample_index >> (8*i)) & 0xFF);
  for (int i=0;i<8;++i) header_buf[16+i] = (uint8_t)((timestamp_us >> (8*i)) & 0xFF);
  header_buf[24] = (uint8_t)(number_of_frames & 0xFF);
  header_buf[25] = (uint8_t)((number_of_frames >> 8) & 0xFF);
  header_buf[26] = (uint8_t)NUMBER_CHANNELS;
  header_buf[27] = (uint8_t)BYTES_PER_SAMPLE;
  header_buf[28] = (uint8_t)(SAMPLE_RATE & 0xFF);
  header_buf[29] = (uint8_t)((SAMPLE_RATE >> 8) & 0xFF);
  header_buf[30] = (uint8_t)((SAMPLE_RATE >> 16) & 0xFF);
  header_buf[31] = (uint8_t)((SAMPLE_RATE >> 24) & 0xFF);
  header_buf[32] = (uint8_t)(FORMAT_INT32_LEFT24 & 0xFF);
  header_buf[33] = (uint8_t)((FORMAT_INT32_LEFT24 >> 8) & 0xFF);
}

// ---------- tasks start/stop ----------
void startTasks() {
  const uint32_t audioStack = 8192; // bytes
  const uint32_t netStack = 4096;   // bytes
  if (audioTaskHandle == NULL) {
    if (xTaskCreatePinnedToCore(audioTask, "audioTask", audioStack, NULL, 6, &audioTaskHandle, 1) != pdPASS) {
      Serial.println("[START] audioTask create failed");
      audioTaskHandle = NULL;
    } else Serial.println("[START] audioTask created");
  }
  if (networkTaskHandle == NULL) {
    if (xTaskCreatePinnedToCore(networkTask, "networkTask", netStack, NULL, 5, &networkTaskHandle, 0) != pdPASS) {
      Serial.println("[START] networkTask create failed");
      networkTaskHandle = NULL;
    } else Serial.println("[START] networkTask created");
  }
}

void stopTasks() {
  consumer_ready.store(false, std::memory_order_release);
  if (TCPCLIENT.connected()) TCPCLIENT.stop();
  if (audioTaskHandle != NULL) { vTaskDelete(audioTaskHandle); audioTaskHandle = NULL; Serial.println("[STOP] audioTask deleted"); }
  if (networkTaskHandle != NULL) { vTaskDelete(networkTaskHandle); networkTaskHandle = NULL; Serial.println("[STOP] networkTask deleted"); }
  delay(50);
}

// ---------- connect helper ----------
static bool connect_to_receiver_ip(const IPAddress &ip, uint16_t port) {
  if (ip == IPAddress(0,0,0,0) || port == 0) return false;
  if (TCPCLIENT.connected()) TCPCLIENT.stop();
  else TCPCLIENT.stop();
  delay(10);
  Serial.printf("[NET] connect -> %s:%u\n", ip.toString().c_str(), (unsigned)port);
  bool ok = TCPCLIENT.connect(ip, port);
  if (!ok || !TCPCLIENT.connected()) { TCPCLIENT.stop(); Serial.println("[NET] connect failed"); return false; }
  TCPCLIENT.setNoDelay(true);
  Serial.println("[NET] connected (handshake done)");
  return true;
}

// ---------- audioTask (producer) ----------
void audioTask(void *pv) {
  (void)pv;
  Serial.printf("[TASK] starting audioTask FRAMES=%u bytesToRead=%u payload=%u\n", FRAMES_PER_PACKET, (unsigned)BYTES_TO_READ, (unsigned)PAYLOAD_BYTES);
  bool paused = false;
  while (true) {
    if (!consumer_ready.load(std::memory_order_acquire)) {
      if (!paused) { Serial.println("[TASK] pausing capture (no receiver connected)"); paused = true; }
      i2s_zero_dma_buffer(I2S_NUM_0);
      vTaskDelay(pdMS_TO_TICKS(200));
      continue;
    } else {
      if (paused) { Serial.println("[TASK] resuming capture (receiver available)"); paused = false; }
    }

    size_t bytes_read = 0;
    esp_err_t r = i2s_read(I2S_NUM_0, (void*)i2s_word_slots, BYTES_TO_READ, &bytes_read, portMAX_DELAY);
    if (r != ESP_OK || bytes_read == 0) { Serial.printf("[I2S] read err %d bytes=%u\n", r, (unsigned)bytes_read); vTaskDelay(pdMS_TO_TICKS(10)); continue; }

    size_t word_count = bytes_read / 4;
    size_t available_frames = (word_count >= NEEDED_WORDS) ? FRAMES_PER_PACKET : (word_count / 2);
    if (available_frames > FRAMES_PER_PACKET) available_frames = FRAMES_PER_PACKET;

    size_t head = ring_head.load(std::memory_order_relaxed);
    size_t tail = ring_tail.load(std::memory_order_acquire);
    size_t nextHead = head + 1;
    if ((nextHead - tail) > RING_SIZE) {
      // ring still full (consumer not keeping up) -> drop newest packet
      absolute_sample_index.fetch_add((uint64_t)available_frames, std::memory_order_relaxed);
      static unsigned drop_count = 0;
      if ((++drop_count % 10) == 0) Serial.printf("[RING] full - dropping pkt head-tail=%u freeHeap=%u\n", (unsigned)(head - tail), (unsigned)esp_get_free_heap_size());
      taskYIELD();
      continue;
    }

    size_t slot = head & RING_MASK;
    if (available_frames == FRAMES_PER_PACKET) {
      for (size_t i=0;i<FRAMES_PER_PACKET;++i) ring_payload[slot][i] = i2s_word_slots[i*2 + 1];
    } else {
      for (size_t i=0;i<available_frames;++i) ring_payload[slot][i] = i2s_word_slots[i*2 + 1];
      for (size_t i=available_frames;i<FRAMES_PER_PACKET;++i) ring_payload[slot][i] = 0;
    }

    uint64_t first_index = absolute_sample_index.load(std::memory_order_relaxed);
    uint64_t ts = (uint64_t)esp_timer_get_time();
    ring_frames[slot] = (uint16_t)available_frames;
    ring_first_index[slot] = first_index;
    ring_timestamp[slot] = ts;
    ring_head.store(nextHead, std::memory_order_release);
    absolute_sample_index.fetch_add((uint64_t)available_frames, std::memory_order_relaxed);
    taskYIELD();
  }
  vTaskDelete(NULL);
}

// ---------- networkTask (consumer) ----------
void networkTask(void *pv) {
  (void)pv;
  Serial.printf("[NET] starting networkTask freeHeap=%u\n", (unsigned)esp_get_free_heap_size());
  IPAddress remoteIp; uint16_t remotePort = 0; unsigned long lastConnectAttempt = 0;
  while (true) {
    gConfig.get(remoteIp, remotePort);

    if (!TCPCLIENT.connected()) {
      unsigned long now = millis();
      if (gConfig.isValid() && WiFi.isConnected() && (now - lastConnectAttempt >= 200)) {
        lastConnectAttempt = now;
        // ensure capture paused before clearing
        consumer_ready.store(false, std::memory_order_release);
        // clear previous buffers & reset indices so we start "fresh" from handshake
        clearRingAndResetIndices();
        if (connect_to_receiver_ip(remoteIp, remotePort)) {
          // now allow capture to start from handshake point
          consumer_ready.store(true, std::memory_order_release);
        } else {
          consumer_ready.store(false, std::memory_order_release);
          vTaskDelay(pdMS_TO_TICKS(50));
          continue;
        }
      } else {
        consumer_ready.store(false, std::memory_order_release);
        vTaskDelay(pdMS_TO_TICKS(20));
        continue;
      }
    }

    // We are connected: send whatever frames are produced after handshake
    size_t tail = ring_tail.load(std::memory_order_acquire);
    size_t head = ring_head.load(std::memory_order_acquire);
    if (tail == head) { // nothing to send yet
      vTaskDelay(pdMS_TO_TICKS(2));
      continue;
    }

    size_t slot = tail & RING_MASK;
    uint16_t frames = ring_frames[slot];
    if (frames == 0 || frames > FRAMES_PER_PACKET) {
      // corrupt or empty slot: advance defensively
      ring_tail.store(tail + 1, std::memory_order_release);
      continue;
    }

    uint32_t seq = sequence_counter.fetch_add(1, std::memory_order_relaxed) + 1;
    uint64_t first_index = ring_first_index[slot];
    uint64_t ts = ring_timestamp[slot];
    write_tcp_header(seq, first_index, ts, (uint16_t)frames);

    size_t hsent = 0; bool ok = true;
    while (hsent < (size_t)HEADER_SIZE) {
      int w = TCPCLIENT.write(header_buf + hsent, HEADER_SIZE - hsent);
      if (w <= 0) { ok = false; break; }
      hsent += (size_t)w;
    }

    if (ok) {
      const uint8_t *p = (const uint8_t*)ring_payload[slot];
      size_t payload_bytes = (size_t)frames * BYTES_PER_SAMPLE * (size_t)NUMBER_CHANNELS;
      size_t sent = 0;
      while (sent < payload_bytes) {
        int w = TCPCLIENT.write(p + sent, payload_bytes - sent);
        if (w <= 0) { ok = false; break; }
        sent += (size_t)w;
        if ((sent % 1024) == 0) taskYIELD();
      }
    }

    if (!ok) {
      Serial.println("[NET] send failed -> closing socket and clearing buffers");
      TCPCLIENT.stop();
      // pause capture then clear ring to avoid replaying stale audio later
      consumer_ready.store(false, std::memory_order_release);
      clearRingAndResetIndices();
      vTaskDelay(pdMS_TO_TICKS(50));
      continue;
    }

    // success -> advance tail
    ring_tail.store(tail + 1, std::memory_order_release);
    taskYIELD();
  }
  vTaskDelete(NULL);
}

// ---------- setup / loop ----------
void setup() {
  Serial.begin(115200);
  while (!Serial) delay(5);
  Serial.println("\n=== corrected ESP32 I2S -> TCP streamer (v13) ===");

  pinMode(BUTTON_PIN, INPUT_PULLUP);

  gConfig.begin();
  setup_wifi_and_params();
  i2s_init();

  // init ring bookkeeping
  clearRingAndResetIndices();

  consumer_ready.store(false);
  startTasks();
  Serial.println("setup done");
}

// detect button hold
bool buttonPressedHold() {
  if (digitalRead(BUTTON_PIN) != LOW) return false;
  unsigned long t0 = millis();
  while (digitalRead(BUTTON_PIN) == LOW) {
    if (millis() - t0 >= BUTTON_HOLD_MS) return true;
    delay(10);
  }
  return false;
}

void loop() {
  static unsigned long last = 0;
  // try to recreate missing tasks periodically (if WiFi up)
  if (millis() - last > 2000) {
    last = millis();
    if (audioTaskHandle == NULL || networkTaskHandle == NULL) {
      Serial.println("[LOOP] detected missing task(s), attempting to (re)create...");
      startTasks();
    }
  }

  if (buttonPressedHold()) {
    startConfigPortalFromButton();
    delay(500);
  }

  // status print every 2s
  static unsigned long statLast = 0;
  if (millis() - statLast > 2000) {
    statLast = millis();
    IPAddress ip; uint16_t port; gConfig.get(ip, port);
    size_t head = ring_head.load(std::memory_order_relaxed), tail = ring_tail.load(std::memory_order_relaxed);
    unsigned occupancy = (head >= tail) ? (unsigned)(head - tail) : 0;
    if (occupancy > (unsigned)RING_SIZE) occupancy = (unsigned)RING_SIZE;
    String s = "[STAT] WiFi="; s += (WiFi.isConnected() ? "OK" : "NO");
    s += " ring="; s += String(occupancy); s += "/"; s += String((unsigned)RING_SIZE);
    s += " seq="; s += String((unsigned)sequence_counter.load(std::memory_order_relaxed));
    s += " sample_idx="; s += String((unsigned long long)absolute_sample_index.load(std::memory_order_relaxed));
    s += " PC="; s += (ip ? ip.toString() : String("(none)")); s += ":"; s += String((unsigned)port);
    s += " freeHeap="; s += String((unsigned)esp_get_free_heap_size());
    s += consumer_ready.load(std::memory_order_relaxed) ? " [consumer=READY]" : " [consumer=NOT_READY]";
    Serial.println(s);
  }

  delay(50);
}
