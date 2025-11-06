#include "main_helper.h"


const char* WIFI_SSID = "94 Pembroke Street - 2";
const char* WiFi_PASS = "welcomehome";


std::shared_ptr<std::atomic<bool>> g_consumer_ready;
std::shared_ptr<std::atomic<size_t>> g_ring_head;
std::shared_ptr<std::atomic<size_t>> g_ring_tail;
std::shared_ptr<std::atomic<uint64_t>> g_abs_idx;
std::shared_ptr<std::atomic<uint32_t>> g_sequence_counter;

static ReciverConfig recivercfg;

static AUDIO_RS audio_rs_instance;

