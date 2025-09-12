// esp_receiver_and_web_combined.cpp
// Single-binary: TCP audio receiver (from ESP streamer) + Drogon web UI
//
// Fixes and improvements compared to original:
//  - correct socket/file types and setsockopt usage
//  - robust recv_all function
//  - correct header parsing with correct byte indices
//  - correct WAV chunk names ("fmt " with trailing space), correct endian writes
//  - thread-safe file writes and WAV header patch on shutdown
//  - integrated Drogon web UI reading/modifying the same shared globals
//  - long descriptive variable names used everywhere for learning clarity
//
// NOTE: this file should be compiled/linked with Drogon and pthreads.
// Use your existing CMake with this file as the receiver + webserver source.
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cmath>
#include <iostream>
#include <cstring>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <drogon/drogon.h>
#include <json/json.h>

using namespace drogon;

// ----------------- Configuration constants -----------------

static const char* SERVER_BIND_ADDRESS = "0.0.0.0";
static const uint16_t TCP_LISTEN_PORT = 7000;   // match ESP PC_PORT
static const uint16_t WEB_HTTP_PORT = 8080;     // web UI port

// Header layout constants (must match the ESP streamer)
static const uint32_t HEADER_MAGIC = 0x45535032; // ASCII 'E' 'S' 'P' '2'
static constexpr int HEADER_SIZE = 34;
static const uint16_t FORMAT_INT32_LEFT24 = 1u;

// Audio parameters (must align with ESP streamer)
static const uint32_t EXPECTED_SAMPLE_RATE = 48000;
static const uint8_t EXPECTED_CHANNEL_COUNT = 1;       // mic channel only
static const uint8_t IN_BYTES_PER_SAMPLE = 4;         // sent as int32 left-aligned
static const uint8_t OUT_BYTES_PER_SAMPLE = 3;        // we write 24-bit WAV -> 3 bytes/sample

// File and buffer sizing
static const char* OUTPUT_WAV_FILENAME = "received_audio_esp32.wav";
static const unsigned BUFFER_SECONDS = 4; // optional ring buffer window (not used in this simple version)
static const size_t RING_BUFFER_SIZE_SAMPLES = (size_t)EXPECTED_SAMPLE_RATE * BUFFER_SECONDS;

// ----------------- Global state (shared between receiver and web UI) -----------------

// These are intentionally long descriptive names for clarity while learning
static std::atomic<bool> global_should_run{true};
static std::atomic<double> global_makeup_gain{1.0};
static std::mutex global_file_write_mutex;
static int global_total_samples_written_count = 0;

static std::atomic<uint64_t> global_highest_received_sample_index{0};
static std::atomic<uint32_t> global_last_received_sequence{0};

// Store listen socket descriptor so signal handler can close it
static int global_tcp_listen_socket_fd = -1;

// ----------------- Utility helpers (endian-safe IO) -----------------

// Write 32-bit little-endian to file
static void write_u32_le(FILE* file_pointer, uint32_t value) {
    uint8_t bytes[4];
    bytes[0] = (uint8_t)(value & 0xFF);
    bytes[1] = (uint8_t)((value >> 8) & 0xFF);
    bytes[2] = (uint8_t)((value >> 16) & 0xFF);
    bytes[3] = (uint8_t)((value >> 24) & 0xFF);
    fwrite(bytes, 1, 4, file_pointer);
}

// Write 16-bit little-endian to file
static void write_u16_le(FILE* file_pointer, uint16_t value) {
    uint8_t bytes[2];
    bytes[0] = (uint8_t)(value & 0xFF);
    bytes[1] = (uint8_t)((value >> 8) & 0xFF);
    fwrite(bytes, 1, 2, file_pointer);
}

// Robustly receive exactly `required_bytes` bytes from `sockfd` into `buffer`.
// Returns true on success, false if connection closed or unrecoverable error.
// This properly handles EINTR and partial reads.
static bool recv_all(int sockfd, uint8_t* buffer, size_t required_bytes) {
    size_t total_received = 0;
    while (total_received < required_bytes) {
        ssize_t r = recv(sockfd, buffer + total_received, required_bytes - total_received, 0);
        if (r < 0) {
            if (errno == EINTR) continue; // interrupted, try again
            perror("recv");
            return false;
        }
        if (r == 0) {
            // peer closed connection
            return false;
        }
        total_received += (size_t)r;
    }
    return true;
}

// Build and write a placeholder WAV header for 24-bit PCM (we will patch sizes later)
// This follows RIFF/WAVE fmt chunk + data chunk with placeholders for sizes.
static bool write_wav_header_placeholder(FILE* file_pointer, uint32_t sample_rate, uint16_t channels) {
    if (!file_pointer) return false;

    // RIFF chunk
    fwrite("RIFF", 1, 4, file_pointer);
    write_u32_le(file_pointer, 0); // placeholder for overall RIFF chunk size
    fwrite("WAVE", 1, 4, file_pointer);

    // fmt chunk (PCM)
    fwrite("fmt ", 1, 4, file_pointer);            // note the trailing space
    write_u32_le(file_pointer, 16);                // fmt chunk size
    write_u16_le(file_pointer, 1);                 // audio format 1 = PCM
    write_u16_le(file_pointer, channels);          // number of channels
    write_u32_le(file_pointer, sample_rate);       // sample rate
    uint32_t byte_rate = sample_rate * channels * OUT_BYTES_PER_SAMPLE;
    write_u32_le(file_pointer, byte_rate);         // byte rate
    uint16_t block_align = channels * OUT_BYTES_PER_SAMPLE;
    write_u16_le(file_pointer, block_align);       // block align
    write_u16_le(file_pointer, OUT_BYTES_PER_SAMPLE * 8); // bits per sample (24)

    // data chunk header
    fwrite("data", 1, 4, file_pointer);
    write_u32_le(file_pointer, 0); // placeholder for data size

    return true;
}

// Finalize WAV header by writing the correct sizes (called on shutdown)
static void finalize_wav_header(FILE* file_pointer, uint32_t total_samples_written, uint16_t channels, uint32_t sample_rate) {
    if (!file_pointer) return;
    uint32_t data_bytes = total_samples_written * (uint32_t)channels * (uint32_t)OUT_BYTES_PER_SAMPLE;
    // riff size = 4 ("WAVE") + (8 + fmtChunkSize) + (8 + dataBytes)
    uint32_t riff_size = 4 + (8 + 16) + (8 + data_bytes);
    fflush(file_pointer);
    fseek(file_pointer, 4, SEEK_SET);
    write_u32_le(file_pointer, riff_size);
    // data chunk size offset is at 4 + (8+16) + 4 = 40
    fseek(file_pointer, 40, SEEK_SET);
    write_u32_le(file_pointer, data_bytes);
    fflush(file_pointer);
}

// Convert a signed 32-bit left-aligned-24 sample to 3 bytes little-endian packed 24-bit
static void int32_left24_to_3bytes_le(int32_t sample_int32_left24, uint8_t out3[3]) {
    // arithmetic right shift by 8 to align 24-bit value
    int32_t sample_24 = sample_int32_left24 >> 8;
    uint32_t u24 = (uint32_t)sample_24 & 0xFFFFFFu;
    out3[0] = (uint8_t)(u24 & 0xFF);
    out3[1] = (uint8_t)((u24 >> 8) & 0xFF);
    out3[2] = (uint8_t)((u24 >> 16) & 0xFF);
}

// Helper to read an int32 from 4 bytes little-endian
static inline int32_t le_bytes_to_int32(const uint8_t* p) {
    uint32_t w = (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
    return (int32_t)w;
}

// ----------------- Header validation helper -----------------
static void validate_header_basics(uint32_t sample_rate, uint8_t channels, uint8_t bytes_per_sample, uint16_t format_id) {
    if (sample_rate != EXPECTED_SAMPLE_RATE) {
        std::cerr << "[WARN] sample_rate mismatch: received=" << sample_rate << " expected=" << EXPECTED_SAMPLE_RATE << "\n";
    }
    if (channels != EXPECTED_CHANNEL_COUNT) {
        std::cerr << "[WARN] channel count mismatch: received=" << int(channels) << " expected=" << int(EXPECTED_CHANNEL_COUNT) << "\n";
    }
    if (bytes_per_sample != IN_BYTES_PER_SAMPLE) {
        std::cerr << "[WARN] bytes_per_sample mismatch: received=" << int(bytes_per_sample) << " expected=" << int(IN_BYTES_PER_SAMPLE) << "\n";
    }
    if (format_id != FORMAT_INT32_LEFT24) {
        std::cerr << "[WARN] format_id mismatch: received=" << format_id << " expected=" << FORMAT_INT32_LEFT24 << "\n";
    }
}

// ----------------- TCP receiver server -----------------

static void tcp_audio_receiver_server_loop() {
    // Create listen socket
    int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0) {
        perror("socket");
        return;
    }

    // keep global fd for shutdown signaling
    global_tcp_listen_socket_fd = listen_fd;

    // allow reuse address
    int reuse_flag = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &reuse_flag, sizeof(reuse_flag));

    struct sockaddr_in bind_address;
    memset(&bind_address, 0, sizeof(bind_address));
    bind_address.sin_family = AF_INET;
    bind_address.sin_port = htons(TCP_LISTEN_PORT);
    bind_address.sin_addr.s_addr = INADDR_ANY;

    if (bind(listen_fd, (struct sockaddr*)&bind_address, sizeof(bind_address)) < 0) {
        perror("bind");
        close(listen_fd);
        global_tcp_listen_socket_fd = -1;
        return;
    }

    if (listen(listen_fd, 1) < 0) {
        perror("listen");
        close(listen_fd);
        global_tcp_listen_socket_fd = -1;
        return;
    }

    std::cout << "[TCP] listening on port " << TCP_LISTEN_PORT << std::endl;

    while (global_should_run.load()) {
        struct sockaddr_in client_address;
        socklen_t client_address_length = sizeof(client_address);

        std::cout << "[TCP] waiting for a client to connect...\n";
        int client_fd = accept(listen_fd, (struct sockaddr*)&client_address, &client_address_length);
        if (client_fd < 0) {
            if (errno == EINTR) {
                // interrupted by signal, check global flag
                continue;
            }
            perror("accept");
            break;
        }

        char client_ip_str[INET_ADDRSTRLEN] = {0};
        inet_ntop(AF_INET, &client_address.sin_addr, client_ip_str, sizeof(client_ip_str));
        std::cout << "[TCP] client connected from " << client_ip_str << ":" << ntohs(client_address.sin_port) << std::endl;

        // Open output WAV file (overwrite existing file on each connect)
        FILE* output_wav_file = nullptr;
        {
            std::lock_guard<std::mutex> lock(global_file_write_mutex);
            output_wav_file = fopen(OUTPUT_WAV_FILENAME, "wb+");
            if (!output_wav_file) {
                perror("fopen");
                close(client_fd);
                continue;
            }
            if (!write_wav_header_placeholder(output_wav_file, EXPECTED_SAMPLE_RATE, EXPECTED_CHANNEL_COUNT)) {
                std::cerr << "[WAV] header write failed\n";
                fclose(output_wav_file);
                close(client_fd);
                continue;
            }
            // seek to end for append
            fseek(output_wav_file, 0, SEEK_END);
        }

        bool connection_ok = true;

        // header buffer
        uint8_t header_buffer[HEADER_SIZE];

        while (global_should_run.load() && connection_ok) {
            // Read header
            if (!recv_all(client_fd, header_buffer, HEADER_SIZE)) {
                std::cout << "[TCP] header receive failed or connection closed\n";
                break;
            }

            // Parse header (little-endian)
            uint32_t magic = (uint32_t)header_buffer[0] | ((uint32_t)header_buffer[1] << 8) |
                             ((uint32_t)header_buffer[2] << 16) | ((uint32_t)header_buffer[3] << 24);
            if (magic != HEADER_MAGIC) {
                std::cerr << "[TCP] bad header magic: 0x" << std::hex << magic << std::dec << std::endl;
                connection_ok = false;
                break;
            }

            uint32_t sequence_number = (uint32_t)header_buffer[4] | ((uint32_t)header_buffer[5] << 8) |
                                       ((uint32_t)header_buffer[6] << 16) | ((uint32_t)header_buffer[7] << 24);

            uint64_t first_sample_index = 0;
            for (int i = 0; i < 8; ++i) first_sample_index |= ((uint64_t)header_buffer[8 + i]) << (8 * i);

            uint64_t timestamp_microseconds = 0;
            for (int i = 0; i < 8; ++i) timestamp_microseconds |= ((uint64_t)header_buffer[16 + i]) << (8 * i);

            uint16_t frames_in_packet = (uint16_t)header_buffer[24] | ((uint16_t)header_buffer[25] << 8);
            uint8_t channels_in_packet = header_buffer[26];
            uint8_t bytes_per_sample_in_packet = header_buffer[27];
            uint32_t sample_rate_in_packet = (uint32_t)header_buffer[28] | ((uint32_t)header_buffer[29] << 8) |
                                             ((uint32_t)header_buffer[30] << 16) | ((uint32_t)header_buffer[31] << 24);
            uint16_t format_id_in_packet = (uint16_t)header_buffer[32] | ((uint16_t)header_buffer[33] << 8);

            // Basic validation (warnings only)
            validate_header_basics(sample_rate_in_packet, channels_in_packet, bytes_per_sample_in_packet, format_id_in_packet);

            // compute payload size
            size_t payload_bytes = (size_t)frames_in_packet * (size_t)channels_in_packet * (size_t)bytes_per_sample_in_packet;
            if (payload_bytes == 0) {
                std::cerr << "[TCP] zero payload size, skipping\n";
                continue;
            }
            // Safety: limit excessive sizes
            const size_t MAX_FRAMES_LIMIT = 1 << 16; // 65536 frames per packet
            if (frames_in_packet == 0 || frames_in_packet > MAX_FRAMES_LIMIT) {
                std::cerr << "[TCP] suspicious number of frames: " << frames_in_packet << " (limiting/skipping)\n";
                connection_ok = false;
                break;
            }

            // Read payload
            std::vector<uint8_t> payload_buffer(payload_bytes);
            if (!recv_all(client_fd, payload_buffer.data(), payload_bytes)) {
                std::cerr << "[TCP] payload receive failed\n";
                connection_ok = false;
                break;
            }

            // Convert each frame's int32 (left-aligned 24) to packed 24-bit and apply gain
            std::vector<uint8_t> output_bytes;
            output_bytes.reserve((size_t)frames_in_packet * OUT_BYTES_PER_SAMPLE * (size_t)channels_in_packet);

            double gain_now = global_makeup_gain.load();
            for (size_t frame_index = 0; frame_index < (size_t)frames_in_packet; ++frame_index) {
                // For mono/one-channel case each frame is 4 bytes (int32)
                size_t base = frame_index * (size_t)bytes_per_sample_in_packet * (size_t)channels_in_packet;
                // for each channel in packet - but we expect single channel
                // read int32 little-endian
                int32_t sample_int32 = le_bytes_to_int32(&payload_buffer[base]);
                // apply gain in double then clamp
                double scaled = (double)sample_int32 * gain_now;
                if (scaled > (double)INT32_MAX) scaled = (double)INT32_MAX;
                if (scaled < (double)INT32_MIN) scaled = (double)INT32_MIN;
                int32_t scaled_i32 = (int32_t)std::llround(scaled);
                // convert left-aligned 32->packed 24
                uint8_t three_bytes[3];
                int32_left24_to_3bytes_le(scaled_i32, three_bytes);
                output_bytes.push_back(three_bytes[0]);
                output_bytes.push_back(three_bytes[1]);
                output_bytes.push_back(three_bytes[2]);
            }

            // Write to WAV file (protected by mutex)
            {
                std::lock_guard<std::mutex> lock(global_file_write_mutex);
                if (!output_wav_file) {
                    std::cerr << "[WAV] output_wav_file null\n";
                } else {
                    size_t wrote_count = fwrite(output_bytes.data(), 1, output_bytes.size(), output_wav_file);
                    if (wrote_count != output_bytes.size()) {
                        std::cerr << "[WAV] short write: " << wrote_count << " of " << output_bytes.size() << "\n";
                    }
                    fflush(output_wav_file);
                    global_total_samples_written_count += (int)frames_in_packet;
                }
            }

            // update status atoms
            global_highest_received_sample_index.store(first_sample_index + (uint64_t)frames_in_packet - 1);
            global_last_received_sequence.store(sequence_number);
        } // end per-packet loop

        // Close client
        close(client_fd);
        std::cout << "[TCP] client disconnected\n";

        // Close and leave header placeholder for finalization on exit
        if (output_wav_file) {
            std::lock_guard<std::mutex> lock(global_file_write_mutex);
            fclose(output_wav_file);
            output_wav_file = nullptr;
        }
    } // accept loop

    if (listen_fd >= 0) close(listen_fd);
    global_tcp_listen_socket_fd = -1;
    std::cout << "[TCP] receiver server exiting\n";
}

// ----------------- Drogon web UI handlers (use the same globals) -----------------

// Build a JSON status object using the shared globals
static Json::Value build_status_json() {
    Json::Value status_json;
    status_json["running"] = global_should_run.load() ? 1 : 0;
    status_json["gain"] = global_makeup_gain.load();
    status_json["last_sequence"] = static_cast<Json::UInt>(global_last_received_sequence.load());
    status_json["highest_sample_index"] = static_cast<Json::UInt64>(global_highest_received_sample_index.load());
    status_json["samples_written"] = global_total_samples_written_count;
    return status_json;
}

// ----------------- Signal handling for graceful shutdown -----------------

static void signal_handler_trigger_shutdown(int /*signum*/) {
    std::cerr << "\nSignal received, initiating graceful shutdown...\n";
    global_should_run.store(false);

    // Close listen socket to break accept()
    if (global_tcp_listen_socket_fd >= 0) {
        shutdown(global_tcp_listen_socket_fd, SHUT_RDWR);
        close(global_tcp_listen_socket_fd);
        global_tcp_listen_socket_fd = -1;
    }

    // Stop drogon event loop (if running)
    try {
        drogon::app().quit();
    } catch (...) {
        // ignore if not running
    }
}

// ----------------- main() - start receiver thread and web server -----------------

int main(int argc, char** argv) {
    (void)argc; (void)argv;
    // Install signal handler for Ctrl-C
    signal(SIGINT, signal_handler_trigger_shutdown);
    signal(SIGTERM, signal_handler_trigger_shutdown);

    // Start the TCP audio receiver in a detached thread
    std::thread tcp_thread(tcp_audio_receiver_server_loop);

    // Configure Drogon (web server)
    drogon::app().addListener(SERVER_BIND_ADDRESS, WEB_HTTP_PORT);

    // Serve index.html (static file) at /
    drogon::app().registerHandler("/", [](const HttpRequestPtr & /*req*/, std::function<void (const HttpResponsePtr &)> &&callback){
        auto response = HttpResponse::newFileResponse("static/index.html");
        callback(response);
    }, {Get});

    // Serve static JS
    drogon::app().registerHandler("/app.js", [](const HttpRequestPtr & /*req*/, std::function<void (const HttpResponsePtr &)> &&callback){
        auto response = HttpResponse::newFileResponse("static/app.js");
        response->addHeader("Content-Type", "application/javascript");
        callback(response);
    }, {Get});

    // Serve static CSS
    drogon::app().registerHandler("/styles.css", [](const HttpRequestPtr & /*req*/, std::function<void (const HttpResponsePtr &)> &&callback){
        auto response = HttpResponse::newFileResponse("static/styles.css");
        response->addHeader("Content-Type", "text/css");
        callback(response);
    }, {Get});

    // GET /status -> return JSON status derived from the shared globals
    drogon::app().registerHandler("/status", [](const HttpRequestPtr & /*req*/, std::function<void (const HttpResponsePtr &)> &&callback){
        Json::Value st = build_status_json();
        auto resp = HttpResponse::newHttpJsonResponse(st);
        callback(resp);
    }, {Get});

    // POST /control -> accept JSON {"gain": <number>} and update global_makeup_gain
    drogon::app().registerHandler("/control", [](const HttpRequestPtr &req, std::function<void (const HttpResponsePtr &)> &&callback){
        try {
            auto json_ptr = req->getJsonObject();
            if (!json_ptr) {
                auto bad = HttpResponse::newHttpResponse();
                bad->setStatusCode(k400BadRequest);
                bad->setBody("Invalid JSON");
                callback(bad);
                return;
            }
            Json::Value &j = *json_ptr;
            if (!j.isMember("gain")) {
                auto bad = HttpResponse::newHttpResponse();
                bad->setStatusCode(k400BadRequest);
                bad->setBody("Missing 'gain' field");
                callback(bad);
                return;
            }
            double requested_gain = j["gain"].asDouble();
            // clamp to reasonable range
            if (requested_gain < 0.01) requested_gain = 0.01;
            if (requested_gain > 16.0) requested_gain = 16.0;
            global_makeup_gain.store(requested_gain);

            Json::Value st = build_status_json();
            auto resp = HttpResponse::newHttpJsonResponse(st);
            callback(resp);
            return;
        } catch (...) {
            auto bad = HttpResponse::newHttpResponse();
            bad->setStatusCode(k400BadRequest);
            bad->setBody("Error parsing JSON");
            callback(bad);
            return;
        }
    }, {Post});

    // Optional background poller that could mirror file/receiver status to the UI (here simply updates local copies)
    std::thread status_poller_thread([](){
        while (global_should_run.load()) {
            // Could do additional monitoring here (e.g., examine file sizes, latency)
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
        }
    });

    // Run Drogon (this will block until app().quit() is called)
    drogon::app().run();

    // Drogon exited - stop poller and join threads
    global_should_run.store(false);
    if (status_poller_thread.joinable()) status_poller_thread.join();

    // Wait for receiver thread to finish (it will exit when global_should_run==false)
    if (tcp_thread.joinable()) tcp_thread.join();

    // Finalize WAV header if file exists
    {
        std::lock_guard<std::mutex> lock(global_file_write_mutex);
        FILE* f = fopen(OUTPUT_WAV_FILENAME, "rb+");
        if (f) {
            finalize_wav_header(f, (uint32_t)global_total_samples_written_count, EXPECTED_CHANNEL_COUNT, EXPECTED_SAMPLE_RATE);
            fclose(f);
            std::cout << "[WAV] finalized header, samples_written=" << global_total_samples_written_count << std::endl;
        } else {
            std::cerr << "[WAV] could not open " << OUTPUT_WAV_FILENAME << " to finalize header\n";
        }
    }

    std::cout << "Shutdown complete.\n";
    return 0;
}