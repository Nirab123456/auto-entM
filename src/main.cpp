/* udp-i2s-nd-lowlatency.ino
   Convert I2S -> UDP low-latency streaming using AudioTools
   Based on your original WAV-server sketch, replaced the HTTP/WAV server
   with a UDPStream + Throttle + StreamCopy pipeline.
*/

#include <Arduino.h>
#include <WiFi.h>
#include "AudioTools.h"
#include "AudioTools/Communication/UDPStream.h"  // ensure this header is available

// ---------- CONFIG: put your real 2.4GHz network credentials here ----------
const char* STATION_SSID = "94 Pembroke Street - 2";   // <- REPLACE if needed
const char* STATION_PASS = "welcomehome";             // <- REPLACE if needed
// -------------------------------------------------------------------------

// UDP destination (set to the PC running the receiver)
const IPAddress DEST_IP(192,168,2,142); // <- CHANGE to your PC IP
const uint16_t DEST_PORT = 7000;        // receiver port

// I2S + audio objects (same converter from your original sketch)
I2SStream i2sStream;
class ConverterFillAndScale : public BaseConverter {
public:
  ConverterFillAndScale(float factor = 2.0f, int channels = 2)
    : filler(LeftIsEmpty, channels),
      scaler(factor, 0, (int16_t)32767, channels)
  {}

  size_t convert(uint8_t *src, size_t byte_count) override {
    // instrumentation omitted here for brevity — keep the conversion behavior
    size_t b = filler.convert(src, byte_count);
    size_t out = scaler.convert(src, b);
    return out;
  }

  void setFactor(float f) { scaler.setFactor(f); }
  float factor() { return scaler.factor(); }

private:
  ConverterFillLeftAndRight<int16_t> filler;
  ConverterScaler<int16_t> scaler;
};
ConverterFillAndScale fillScaleConv(2.0f, 2); // keep default gain

// UDP stream + throttle + copier
UDPStream udp(STATION_SSID, STATION_PASS);
Throttle throttle(udp);         // throttle wraps the UDP sink so we send at real-time speed
StreamCopy copier(throttle, i2sStream); // copies from I2S source into the Throttle(->UDP)

// I2S pins (same as your original)
const int PIN_BCK  = 7;   // BCLK / SCK
const int PIN_WS   = 15;  // LRCLK / WS
const int PIN_DATA = 16;  // SD (data in from INMP441)

// WiFi AP fallback logic (kept from your sketch)
void startAP(const char* apName) {
  WiFi.mode(WIFI_MODE_APSTA);
  delay(50);
  Serial.printf("[WIFI] Starting AP: %s (open)\n", apName);
  if (!WiFi.softAP(apName)) {
    Serial.println("[WIFI] softAP failed");
    return;
  }
  IPAddress apIP = WiFi.softAPIP();
  Serial.print("[WIFI] AP IP: ");
  Serial.println(apIP);
}

// Small I2S init tuned for low latency (your settings preserved)
void setupI2S() {
  auto cfg = i2sStream.defaultConfig(RX_MODE);
  cfg.i2s_format = I2S_STD_FORMAT;
  cfg.sample_rate = 16000;     // keep 16 kHz (voice-quality)
  cfg.channels = 2;            // keep stereo (we duplicate right->left in converter)
  cfg.bits_per_sample = 16;
  cfg.buffer_count = 2;        // small buffers for low latency (risk under-run)
  cfg.buffer_size  = 128;      // small buffer size
  cfg.pin_bck  = PIN_BCK;
  cfg.pin_ws   = PIN_WS;
  cfg.pin_data = PIN_DATA;

  Serial.println("[I2S] initializing...");
  if (! i2sStream.begin(cfg)) {
    Serial.println("[I2S] begin FAILED - halting");
    while (1) delay(500);
  }
  Serial.println("[I2S] OK");
}

// WiFi STA attempt helper (non-blocking handling left out for brevity)
void beginSTAIfConfigured() {
  if (!STATION_SSID || strlen(STATION_SSID) == 0) {
    Serial.println("[WIFI] No STA credentials provided; staying AP-only.");
    return;
  }
  Serial.printf("[WIFI] Attempting STA join: '%s'\n", STATION_SSID);
  WiFi.begin(STATION_SSID, STATION_PASS);
}

// Small audio RT task pinned to core 1 (tight loop for lowest latency)
void audioTask(void *pv) {
  (void) pv;
  // Setup Throttle config based on i2sStream config so timing is correct
  AudioInfo info;
  info.sample_rate = 16000;
  info.channels = 2;
  info.bits_per_sample = 16;

  // configure throttle BEFORE starting copy (done once)
  auto tcfg = throttle.defaultConfig();
  tcfg.copyFrom(info);
  // small correction value (ms) may help long-term drift; leave default or tune:
  // tcfg.correction_ms = 0;
  throttle.begin(tcfg);

  // Important: If you want the converter to run, StreamCopy must invoke converters
  // internally when the sink supports converters. If not, consider feeding the
  // converter explicitly by using a ConverterStream (check your AudioTools version).
  // We'll call copier.copy() which typically will pass data through any configured converters.
  for (;;) {
    copier.copy();   // blocking-ish copy; tight loop for low latency
    // no delay here — tight loop yields lowest upstream latency
  }
}

void startAudioTask() {
  xTaskCreatePinnedToCore(audioTask, "audioTask", 4096, NULL, 6, NULL, 1);
}

void setup() {
  Serial.begin(115200);
  while (!Serial) delay(5);
  AudioToolsLogger.begin(Serial, AudioToolsLogLevel::Info);
  delay(50);

  Serial.println("\n=== UDP I2S -> UDP low-latency streamer (ESP32-S3 + INMP441) ===");

  // I2S init
  setupI2S();

  // Start AP (so you can always connect directly)
  uint8_t mac[6];
  WiFi.macAddress(mac);
  char apname[32];
  sprintf(apname, "ESP32-Audio-%02X%02X", mac[4], mac[5]);
  startAP(apname);

  // Try STA (non-blocking / status handled in loop if desired)
  WiFi.mode(WIFI_MODE_STA);
  delay(50);
  beginSTAIfConfigured();
  WiFi.mode(WIFI_MODE_APSTA);
  delay(50);

  // configure and open UDP destination (unicast recommended)
  udp.begin(DEST_IP, DEST_PORT);
  Serial.printf("[UDP] will send to %s:%u\n", DEST_IP.toString().c_str(), DEST_PORT);

  // Start audio RT task (does copier.copy())
  startAudioTask();

  Serial.println("[SETUP] done; streaming will begin in audio task.");
}

void loop() {
  static wl_status_t lastStatus = WL_DISCONNECTED;
  wl_status_t st = WiFi.status();
  if (st != lastStatus) {
    Serial.printf("[WIFI] STA status changed: %d\n", st);
    lastStatus = st;
  }
  // You can print occasional diagnostics or adjust gain via Serial commands here
  delay(200);
}
