// include/shared_state.h
#pragma once
#include <atomic>
#include <cstdint>

extern std::atomic<double> g_gain;
extern std::atomic<uint32_t> g_last_seq;
extern std::atomic<uint64_t> g_highest_received_index;
extern int g_total_samples_written;
