// main.cpp
#include <Arduino.h>
#include <memory>
#include <atomic>
#include <span>
#include "headers/audio_read_send.h"   // your header
#include "headers/a_c_s.h"   // defines FRAMES_PER_PACKET, RING_SIZE, BYTES_PER_SAMPLE, etc.

//
// Globals backing the spans (must outlive tasks). Keep them static or file-scope.
//
constexpr size_t RING_SLOTS = RING_SIZE;                     // from your config
constexpr size_t FRAMES = FRAMES_PER_PACKET;                 // from your config
static uint32_t RING_PAYLOAD[RING_SLOTS][FRAMES];            // big 2D array (contiguous)
static uint32_t I2S_WORD_SLOTS[FRAMES * DEFAULT_CHANNEL_COUNT]; // i2s temp buffer

// Shared atomic state (wrapped in shared_ptr to pass ownership to AUDIO_RS)
static auto consumer_ready_sp = std::make_shared<std::atomic<bool>>(false);
static auto ring_head_sp      = std::make_shared<std::atomic<size_t>>(0);
static auto ring_tail_sp      = std::make_shared<std::atomic<size_t>>(0);
static auto abs_idx_sp        = std::make_shared<std::atomic<uint64_t>>(0);

// Make a static AUDIO_RS instance so it lives forever (task pv expects pointer to it)
static AUDIO_RS audio(
    /* i2s_buffer = */ std::span<uint32_t>(I2S_WORD_SLOTS, sizeof(I2S_WORD_SLOTS)/sizeof(I2S_WORD_SLOTS[0])),
    /* ring_payload_flat = */ std::span<uint32_t>(&RING_PAYLOAD[0][0], RING_SLOTS * FRAMES),
    /* frames per packet */ FRAMES,
    /* consumer_ready */ consumer_ready_sp,
    /* ring_head */ ring_head_sp,
    /* ring_tail */ ring_tail_sp,
    /* abs_idx */ abs_idx_sp
);

void setup() {
  Serial.begin(115200);
  delay(100);

  Serial.println("Audio system starting...");

  // Optionally tune policy or other settings here:
  // audio.ovverrunpolicy_newest(); // or audio.ovverrunpolicy_oldest();

  // Start the reader (I2S) task
  bool ok1 = audio.start_task(
    "I2SReader",           // name shown in FreeRTOS
    4096,                  // task stack in bytes (adjust)
    3,                     // priority (higher number => higher priority)
    1,                     // pinned core (use 0 or 1; use -1 for unpinned if you prefer)
    AUDIO_RS::I2SReadTrampoline, // trampoline function declared in header
    nullptr                // argument passed to trampoline (we want 'this' by default)
  );
  Serial.printf("I2SReader started: %d\n", ok1 ? 1 : 0);

  // Start the ring writer task
  bool ok2 = audio.start_task(
    "RingWriter",
    8192,                 // writer needs larger stack for buffer ops
    2,                    // slightly lower priority than reader
    1,
    AUDIO_RS::RingWriterFRMI2STrampoline,
    nullptr
  );
  Serial.printf("RingWriter started: %d\n", ok2 ? 1 : 0);

  // Start optional fingerprint & network tasks (if you implemented them)
  bool ok3 = audio.start_task(
    "Fingerprint",
    8192,
    1,
    1,
    AUDIO_RS::AudioTaskTrampoline, // or a dedicated Fingerprint trampoline if you have one
    nullptr
  );
  Serial.printf("Fingerprint started: %d\n", ok3 ? 1 : 0);

  bool ok4 = audio.start_task(
    "Network",
    4096,
    1,
    1,
    AUDIO_RS::NetworkTaskTrampoline,
    nullptr
  );
  Serial.printf("Network started: %d\n", ok4 ? 1 : 0);

  // Kick system into "ready" — consumer flag can be used to pause/resume writer behavior.
  consumer_ready_sp->store(true, std::memory_order_release);
}

void loop() {
  // In loop we can check button press, serial commands, or scheduled resets.
  // Example: reset the ring if user sends "r" on serial, or every 60s for demo:
  static unsigned long last_reset = 0;
  if (millis() - last_reset > 60000) { // reset every 60s just as an example
      Serial.println("Main: performing periodic reset of ring & counters");
      audio.Ring_clear_Rst(); // calls Ring_clear_Rst() internally
      last_reset = millis();
  }

  // Example: handle serial commands
  if (Serial.available()) {
      String cmd = Serial.readStringUntil('\n');
      if (cmd == "reset") {
          Serial.println("Manual reset requested");
          audio.Ring_clear_Rst();
      } else if (cmd == "pause") {
          consumer_ready_sp->store(false, std::memory_order_release);
      } else if (cmd == "resume") {
          consumer_ready_sp->store(true, std::memory_order_release);
      }
      // log drop counters (if you add getters)
      // Serial.printf("drop_newest=%u drop_oldest=%u\n", audio.get_drop_count_newest(), audio.get_drop_count_oldest());
  }

  // keep loop light, all real work is in tasks
  delay(100);
}
