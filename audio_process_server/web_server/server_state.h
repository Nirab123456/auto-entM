// web-server/server_state.h
#pragma once

#include <atomic>
#include <mutex>

// These must match names & types in your receiver translation unit (code_receiver_fixed.cpp).
// In your receiver file, make sure the variables are non-static and have external linkage.

extern std::atomic<bool> g_running;
extern std::atomic<double> g_gain;
extern std::mutex g_file_mutex;
extern int g_total_samples_written;
extern std::atomic<uint64_t> g_highest_received_index;
extern std::atomic<uint32_t> g_last_seq;

// Path to static files (optional); used by the web server to serve files relative to the binary.
extern const char *STATIC_FILES_DIR;
