// web-server/server_state.h
#pragma once

#include <atomic>
#include <mutex>

// These must match names & types in your receiver translation unit (code_receiver_fixed.cpp).
// In your receiver file, make sure the variables are non-static and have external linkage.

extern std::atomic<bool> global_running;
extern std::atomic<double> global_gain;
extern std::mutex global_file_mutex;
extern int global_total_samples_written;
extern std::atomic<uint64_t> global_highest_recived_idx;
extern std::atomic<uint32_t> global_last_sequence;

// Path to static files (optional); used by the web server to serve files relative to the binary.
extern const char *STATIC_FILES_DIR;
