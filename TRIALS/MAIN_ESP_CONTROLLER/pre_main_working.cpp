// corrected_esp32_stream_tcp_i2s_fixed_v8.ino
// v8: avoid backlog blowup by dropping newest packet when ring full,
//     RING_SIZE increased to 64, safe prints, large buffers in BSS.

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiManager.h>
#include "driver/i2s.h"
#include "esp_timer.h"
#include <Preferences.h>
#include <atomic>

// ---------- user configuration ----------
const char* WIFI_SSID = "94 Pembroke Street - 2";
const char* WIFI_PASS = "welcomehome";

char PC_IP_BUF[40] = "";          // stored IP string (no String across tasks)
std::atomic<uint16_t> PC_PORT{7000};

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
const size_t NEEDED_WORDS   = (size_t)FRAMES_PER_PACKET * 2; // words (32-bit) needed

// header constants (must match receiver)
constexpr int HEADER_SIZE = 34;
const uint32_t HEADER_MAGIC = 0x45535032; // 'ESP2'
const uint16_t FORMAT_INT32_LEFT24 = 1;

// ring buffer parameters (increased)
constexpr size_t RING_SIZE = 64; // changed from 8 -> 64
constexpr size_t RING_MASK = RING_SIZE - 1;

// ---------- LARGE BUFFERS (file scope -> BSS) ----------
static uint32_t i2s_word_slots[FRAMES_PER_PACKET * 2]; // L0,R0,L1,R1 ... (global)
static uint32_t payload_words[FRAMES_PER_PACKET];      // extracted right-channel words (global)

static uint32_t ring_payload[RING_SIZE][FRAMES_PER_PACKET]; // payload words per slot
static uint16_t ring_frames[RING_SIZE];                    // frames in slot
static uint64_t ring_first_index[RING_SIZE];
static uint64_t ring_timestamp[RING_SIZE];

// atomics for ring indices
std::atomic<size_t> ring_head{0}; // next write index (published after write)
std::atomic<size_t> ring_tail{0}; // next read index

// networking client (only used by networkTask)
WiFiClient TCPCLIENT;

// atomics for sequence and sample indices
std::atomic<uint32_t> sequence_counter{0};
std::atomic<uint64_t> absolute_sample_index{0};

// header buffer (global)
static uint8_t header_buf[HEADER_SIZE];

Preferences prefs;
WiFiManager wm;

// ---------- helpers ----------
static bool looks_like_ip(const char* s) {
  int a,b,c,d; char tail;
  int n = sscanf(s, "%d.%d.%d.%d%c", &a,&b,&c,&d,&tail);
  if (n == 4) return (a>=0 && a<=255 && b>=0 && b<=255 && c>=0 && c<=255 && d>=0 && d<=255);
  return false;
}
static bool looks_like_port(const char* s) {
  long p = atol(s);
  return (p > 0 && p <= 65535);
}

void load_prefs() {
  prefs.begin("config", true);
  String saved_ip = prefs.getString("pc_ip", "");
  String saved_port = prefs.getString("pc_port", "");
  if (saved_ip.length() && looks_like_ip(saved_ip.c_str())) saved_ip.toCharArray(PC_IP_BUF, sizeof(PC_IP_BUF));
  if (saved_port.length() && looks_like_port(saved_port.c_str())) PC_PORT.store((uint16_t)saved_port.toInt(), std::memory_order_relaxed);
  prefs.end();
}
void save_prefs(const char* ip, uint16_t port) {
  prefs.begin("config", false);
  prefs.putString("pc_ip", String(ip));
  prefs.putString("pc_port", String(port));
  prefs.end();
}

void setup_wifi_and_params() {
  load_prefs();

  char ipbuf[40]; ipbuf[0]=0;
  char portbuf[16]; portbuf[0]=0;
  if (PC_IP_BUF[0]) strncpy(ipbuf, PC_IP_BUF, sizeof(ipbuf));
  uint16_t port_local = PC_PORT.load(std::memory_order_relaxed);
  if (port_local) snprintf(portbuf, sizeof(portbuf), "%u", (unsigned)port_local);

  WiFiManagerParameter ip_param("pcip", "Receiver IP (e.g. 192.168.2.133)", ipbuf, sizeof(ipbuf));
  WiFiManagerParameter port_param("pcport", "Receiver Port (e.g. 7000)", portbuf, sizeof(portbuf));
  wm.addParameter(&ip_param);
  wm.addParameter(&port_param);

  Serial.println("[WIFI] starting WiFiManager autoConnect (opens AP if needed)...");
  bool res = wm.autoConnect(WIFI_AP_NAME, WIFI_AP_PASS);
  if (!res) {
    Serial.println("[WIFI] WiFiManager failed or user cancelled.");
  } else {
    Serial.println("[WIFI] Connected to WiFi via WiFiManager.");
    const char* ipv = ip_param.getValue();
    const char* portv = port_param.getValue();
    if (ipv && ipv[0] && looks_like_ip(ipv)) {
      strncpy(PC_IP_BUF, ipv, sizeof(PC_IP_BUF)); PC_IP_BUF[sizeof(PC_IP_BUF)-1]=0;
    }
    if (portv && portv[0] && looks_like_port(portv)) {
      PC_PORT.store((uint16_t)atoi(portv), std::memory_order_relaxed);
    }
    save_prefs(PC_IP_BUF, PC_PORT.load(std::memory_order_relaxed));
    Serial.print("[WIFI] saved PC IP=");
    Serial.println(PC_IP_BUF[0] ? PC_IP_BUF : "(none)");
    Serial.print("[WIFI] saved PC PORT=");
    Serial.println((unsigned)PC_PORT.load(std::memory_order_relaxed));
  }
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
    Serial.print("[I2S] driver_install failed: ");
    Serial.println((int)err);
    while(1) delay(500);
  }
  err = i2s_set_pin(I2S_NUM_0, &pin_config);
  if (err != ESP_OK) {
    Serial.print("[I2S] set_pin failed: ");
    Serial.println((int)err);
    while(1) delay(500);
  }
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

// ---------- audioTask (producer) ----------
void audioTask(void *pv) {
  (void)pv;
  Serial.print("[TASK] starting audioTask FRAMES=");
  Serial.print((unsigned)FRAMES_PER_PACKET);
  Serial.print(" bytesToRead=");
  Serial.print((unsigned)BYTES_TO_READ);
  Serial.print(" payload=");
  Serial.println((unsigned)PAYLOAD_BYTES);

  while (true) {
    // read from I2S (blocking)
    size_t bytes_read = 0;
    esp_err_t res = i2s_read(I2S_NUM_0, (void*)i2s_word_slots, BYTES_TO_READ, &bytes_read, portMAX_DELAY);
    if (res != ESP_OK || bytes_read == 0) {
      Serial.print("[I2S] read err ");
      Serial.print((int)res);
      Serial.print(" bytes=");
      Serial.println((unsigned)bytes_read);
      vTaskDelay(pdMS_TO_TICKS(10));
      continue;
    }

    size_t word_count = bytes_read / 4;
    size_t available_frames = (word_count >= NEEDED_WORDS) ? FRAMES_PER_PACKET : (word_count / 2);
    if (available_frames > FRAMES_PER_PACKET) available_frames = FRAMES_PER_PACKET; // defensive

    // check ring capacity: *do not* advance head if full -> drop newest
    size_t head = ring_head.load(std::memory_order_relaxed);
    size_t tail = ring_tail.load(std::memory_order_acquire);
    size_t nextHead = head + 1;

    if ((nextHead - tail) > RING_SIZE) {
      // ring full -> drop newest packet to keep latency bounded
      // advance sample index because we've consumed audio but will not send it
      absolute_sample_index.fetch_add((uint64_t)available_frames, std::memory_order_relaxed);
      // optional lightweight debug every N drops (don't spam)
      static unsigned drop_count = 0;
      if ((++drop_count % 10) == 0) {
        Serial.print("[RING] full - dropping packet (head-tail=");
        Serial.print((unsigned)(head - tail));
        Serial.print(") freeHeap=");
        Serial.println((unsigned)esp_get_free_heap_size());
      }
      // yield and continue (skip enqueueing)
      taskYIELD();
      continue;
    }

    // reserve the slot (single producer, safe)
    size_t slot = head & RING_MASK;

    // copy payload into reserved slot
    if (available_frames == FRAMES_PER_PACKET) {
      for (size_t i = 0; i < FRAMES_PER_PACKET; ++i) ring_payload[slot][i] = i2s_word_slots[i * 2 + 1];
    } else {
      for (size_t i = 0; i < available_frames; ++i) ring_payload[slot][i] = i2s_word_slots[i * 2 + 1];
      for (size_t i = available_frames; i < FRAMES_PER_PACKET; ++i) ring_payload[slot][i] = 0;
    }

    // metadata
    uint64_t first_index = absolute_sample_index.load(std::memory_order_relaxed);
    uint64_t ts = (uint64_t)esp_timer_get_time();

    ring_frames[slot] = (uint16_t)available_frames;
    ring_first_index[slot] = first_index;
    ring_timestamp[slot] = ts;

    // publish head
    ring_head.store(nextHead, std::memory_order_release);

    // advance the sample index (we consumed these samples)
    absolute_sample_index.fetch_add((uint64_t)available_frames, std::memory_order_relaxed);

    taskYIELD();
  }
  vTaskDelete(NULL);
}

// ---------- networkTask (consumer) ----------
void networkTask(void *pv) {
  (void)pv;
  Serial.print("[NET] starting networkTask freeHeap=");
  Serial.println((unsigned)esp_get_free_heap_size());

  char local_ip[40];
  uint16_t local_port = 0;

  while (true) {
    size_t tail = ring_tail.load(std::memory_order_acquire);
    size_t head = ring_head.load(std::memory_order_acquire);

    if (tail == head) {
      // nothing to send
      vTaskDelay(pdMS_TO_TICKS(2));
      continue;
    }

    size_t slot = tail & RING_MASK;
    uint16_t frames = ring_frames[slot];
    if (frames == 0 || frames > FRAMES_PER_PACKET) {
      // corrupted/invalid -> advance tail defensively
      ring_tail.store(tail + 1, std::memory_order_release);
      continue;
    }

    // copy local PC IP/port
    strncpy(local_ip, PC_IP_BUF, sizeof(local_ip));
    local_ip[sizeof(local_ip)-1] = 0;
    local_port = PC_PORT.load(std::memory_order_relaxed);

    // ensure TCP connected
    if (!TCPCLIENT.connected()) {
      if (!local_ip[0] || local_port == 0 || !WiFi.isConnected()) {
        vTaskDelay(pdMS_TO_TICKS(20));
        continue;
      }
      Serial.print("[NET] connecting to ");
      Serial.print(local_ip);
      Serial.print(":");
      Serial.println((unsigned)local_port);
      if (!TCPCLIENT.connect(local_ip, local_port)) {
        Serial.println("[NET] connect failed, retrying");
        TCPCLIENT.stop();
        vTaskDelay(pdMS_TO_TICKS(200));
        continue;
      } else {
        TCPCLIENT.setNoDelay(true);
        Serial.println("[NET] connected");
      }
    }

    // compose header
    uint32_t seq = sequence_counter.fetch_add(1, std::memory_order_relaxed) + 1;
    uint64_t first_index = ring_first_index[slot];
    uint64_t ts = ring_timestamp[slot];
    write_tcp_header(seq, first_index, ts, (uint16_t)frames);

    // send header
    size_t hsent = 0;
    bool ok = true;
    while (hsent < (size_t)HEADER_SIZE) {
      int w = TCPCLIENT.write(header_buf + hsent, HEADER_SIZE - hsent);
      if (w <= 0) { ok = false; break; }
      hsent += (size_t)w;
    }

    // send payload
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
      Serial.println("[NET] send failed, closing client");
      TCPCLIENT.stop();
      // drop this slot to avoid spinning on it
      ring_tail.store(tail + 1, std::memory_order_release);
    } else {
      // success -> advance tail
      ring_tail.store(tail + 1, std::memory_order_release);
    }

    taskYIELD();
  }
  vTaskDelete(NULL);
}

// ---------- setup/loop and task starters ----------
void startTasks() {
  // audioTask pinned to core 1
  const uint32_t audioStack = 16384; // bytes
  if (xTaskCreatePinnedToCore(audioTask, "audioTask", audioStack, NULL, 6, NULL, 1) != pdPASS) {
    Serial.println("[START] audioTask create failed");
  } else {
    Serial.println("[START] audioTask created");
  }

  // networkTask pinned to core 0
  const uint32_t netStack = 8192;
  if (xTaskCreatePinnedToCore(networkTask, "networkTask", netStack, NULL, 5, NULL, 0) != pdPASS) {
    Serial.println("[START] networkTask create failed");
  } else {
    Serial.println("[START] networkTask created");
  }
}

void setup() {
  Serial.begin(115200);
  while (!Serial) delay(5);
  Serial.println("\n=== corrected ESP32 I2S -> TCP streamer (v8 drop-newest) ===");

  setup_wifi_and_params();
  i2s_init();

  // init ring bookkeeping
  ring_head.store(0);
  ring_tail.store(0);
  for (size_t i=0;i<RING_SIZE;++i) {
    ring_frames[i] = 0;
    ring_first_index[i] = 0;
    ring_timestamp[i] = 0;
  }

  startTasks();
  Serial.println("setup done");
}

void loop() {
  static unsigned long last = 0;
  if (millis() - last > 2000) {
    last = millis();
    String status = "[STAT] WiFi=";
    status += (WiFi.isConnected() ? "OK" : "NO");
    status += " ring=";
    size_t head = ring_head.load(std::memory_order_relaxed);
    size_t tail = ring_tail.load(std::memory_order_relaxed);
    // print occupancy but cap to RING_SIZE for clarity
    unsigned occupancy = 0;
    if (head >= tail) occupancy = (unsigned)(head - tail);
    else occupancy = 0;
    if (occupancy > (unsigned)RING_SIZE) occupancy = (unsigned)RING_SIZE;
    status += String(occupancy);
    status += "/";
    status += String((unsigned)RING_SIZE);
    status += " seq=";
    status += String((unsigned)sequence_counter.load(std::memory_order_relaxed));
    status += " sample_idx=";
    status += String((unsigned long long)absolute_sample_index.load(std::memory_order_relaxed));
    status += " PC=";
    status += (PC_IP_BUF[0] ? PC_IP_BUF : "(none)");
    status += ":";
    status += String((unsigned)PC_PORT.load(std::memory_order_relaxed));
    status += " freeHeap=";
    status += String((unsigned)esp_get_free_heap_size());
    Serial.println(status);
  }
  delay(100);
}
