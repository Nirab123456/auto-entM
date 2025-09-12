// code_reciver_fixed.cpp
// Minimal portable TCP receiver for the ESP streamer.
// Fixed: added missing headers (<vector>, <cmath>) and a couple safety includes.

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
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
#include <cstring>
#include <cmath>       // <-- needed for llround
#include <iostream>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>      // <-- needed for std::vector

// ----------------- Config -----------------
static const char* LISTEN_ADDR = "0.0.0.0";
static const uint16_t LISTEN_PORT = 7000; // match ESP PC_PORT
static const uint16_t HTTP_PORT = 8080;   // control UI

// Must match ESP header layout
static const uint32_t HEADER_MAGIC = 0x45535032; // 'ESP2'
static const int HEADER_SIZE = 34;
static const uint16_t FORMAT_INT32_LEFT24 = 1u;

// Audio params (must align with ESP streamer)
static const uint32_t SAMPLE_RATE = 48000;
static const uint8_t CHANNELS    = 1;   // we're receiving only the mic channel
static const uint8_t IN_BYTES_PER_SAMPLE  = 4; // from ESP (int32 left-aligned)
static const uint8_t OUT_BYTES_PER_SAMPLE = 3; // WAV 24-bit

// File & buffer sizing
static const char* OUT_FILENAME = "received_high_quality.wav";

// Keep ring buffer optional — set to 0 to disable ring buffering (direct write)
static const unsigned BUFFER_SECONDS = 4; // reduce for low-RAM devices
static const size_t RING_SIZE = (size_t)SAMPLE_RATE * BUFFER_SECONDS; // samples

// ----------------- Global state -----------------
std::atomic<bool> g_running(true);
std::atomic<double> g_gain(1.0); // makeup gain applied before writing
std::mutex g_file_mutex;
int g_total_samples_written = 0; // for WAV header fixup

// Simple status
std::atomic<uint64_t> g_highest_received_index(0);
std::atomic<uint32_t> g_last_seq(0);

// ----------------- Helpers -----------------

// receive exactly n bytes, handling partial reads
// returns true if success, false on EOF/error
bool recv_all(int sockfd, uint8_t* buffer, size_t n) {
    size_t got = 0;
    while (got < n) {
        ssize_t r = recv(sockfd, buffer + got, (n - got), 0);
        if (r < 0) {
            if (errno == EINTR) continue;
            perror("recv");
            return false;
        }
        if (r == 0) return false; // connection closed
        got += (size_t)r;
    }
    return true;
}

// write little-endian 32-bit
void write_u32_le(FILE* f, uint32_t v) {
    uint8_t b[4];
    b[0] = v & 0xFF; b[1] = (v >> 8) & 0xFF; b[2] = (v >> 16) & 0xFF; b[3] = (v >> 24) & 0xFF;
    fwrite(b, 1, 4, f);
}
// write little-endian 16-bit
void write_u16_le(FILE* f, uint16_t v) {
    uint8_t b[2];
    b[0] = v & 0xFF; b[1] = (v >> 8) & 0xFF;
    fwrite(b, 1, 2, f);
}

// create WAV header placeholder (we will patch sizes later)
// format: PCM 24-bit WAV (RIFF)
bool write_wav_header_placeholder(FILE* f, uint32_t sample_rate, uint16_t channels) {
    if (!f) return false;
    // RIFF header
    fwrite("RIFF", 1, 4, f);
    write_u32_le(f, 0); // placeholder for chunk size
    fwrite("WAVE", 1, 4, f);

    // fmt chunk (PCM)
    fwrite("fmt ", 1, 4, f);
    write_u32_le(f, 16); // fmt chunk size
    write_u16_le(f, 1);  // audio format 1 = PCM (we'll use packed 24-bit).
    write_u16_le(f, channels);
    write_u32_le(f, sample_rate);
    uint32_t byte_rate = sample_rate * channels * OUT_BYTES_PER_SAMPLE;
    write_u32_le(f, byte_rate);
    uint16_t block_align = channels * OUT_BYTES_PER_SAMPLE;
    write_u16_le(f, block_align);
    write_u16_le(f, OUT_BYTES_PER_SAMPLE * 8); // bits per sample (24)

    // data chunk header
    fwrite("data", 1, 4, f);
    write_u32_le(f, 0); // placeholder for data chunk size

    return true;
}

// finalize WAV header with actual sizes
void finalize_wav_header(FILE* f, uint32_t total_samples, uint16_t channels, uint32_t sample_rate) {
    // Only handle 24-bit PCM (3 bytes/sample)
    uint32_t data_bytes = total_samples * channels * OUT_BYTES_PER_SAMPLE;
    uint32_t riff_size = 4 + (8 + 16) + (8 + data_bytes); // "WAVE" + fmt chunk + data chunk
    fflush(f);
    fseek(f, 4, SEEK_SET);
    write_u32_le(f, riff_size);
    fseek(f, 40, SEEK_SET); // data chunk size offset: 4 + (8+16) + 4 + 4 = 40
    write_u32_le(f, data_bytes);
    fflush(f);
}

// convert one int32 (little-endian) left-aligned 24-bit to 3 bytes little-endian
// input: int32_t sample (native endian already parsed); returns 3 bytes in buffer[3]
void int32_to_24le_bytes(int32_t s32, uint8_t out3[3]) {
    // samples come as left-aligned 24-bit in 32-bit signed value
    // arithmetic right-shift by 8 to align to 24-bit signed value
    int32_t s24 = s32 >> 8; // arithmetic shift keeps sign
    // pack lower 3 bytes little-endian
    uint32_t u = (uint32_t)s24 & 0xFFFFFFu;
    out3[0] = u & 0xFF;
    out3[1] = (u >> 8) & 0xFF;
    out3[2] = (u >> 16) & 0xFF;
}

// safe convert from raw payload bytes to little-endian int32_t (assuming payload little-endian)
inline int32_t le_bytes_to_int32(const uint8_t* p) {
    uint32_t w = (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
    return (int32_t)w;
}

// ----------------- TCP server thread -----------------
void tcp_server_loop() {
    int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0) {
        perror("socket");
        return;
    }
    int reuse = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    struct sockaddr_in addr;
    addr.sin_family = AF_INET;
    addr.sin_port = htons(LISTEN_PORT);
    addr.sin_addr.s_addr = INADDR_ANY;
    if (bind(listen_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("bind");
        close(listen_fd);
        return;
    }
    if (listen(listen_fd, 1) < 0) {
        perror("listen");
        close(listen_fd);
        return;
    }
    std::cout << "[TCP] listening on port " << LISTEN_PORT << std::endl;

    while (g_running.load()) {
        struct sockaddr_in cli;
        socklen_t cli_len = sizeof(cli);
        std::cout << "[TCP] waiting for client..." << std::endl;
        int cli_fd = accept(listen_fd, (struct sockaddr*)&cli, &cli_len);
        if (cli_fd < 0) {
            if (errno == EINTR) continue;
            perror("accept");
            break;
        }
        char cli_ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &cli.sin_addr, cli_ip, sizeof(cli_ip));
        std::cout << "[TCP] client connected from " << cli_ip << ":" << ntohs(cli.sin_port) << std::endl;

        // open file (append mode). Use mutex to protect from HTTP control thread finalization.
        FILE* out = nullptr;
        {
            std::lock_guard<std::mutex> lk(g_file_mutex);
            // On first connection create file and write header placeholder
            out = fopen(OUT_FILENAME, "wb+");
            if (!out) {
                perror("fopen");
                close(cli_fd);
                continue;
            }
            if (!write_wav_header_placeholder(out, SAMPLE_RATE, CHANNELS)) {
                std::cerr << "[WAV] header write failed\n";
                fclose(out);
                close(cli_fd);
                continue;
            }
            // Move to end to append data
            fseek(out, 0, SEEK_END);
        }

        bool conn_ok = true;
        // allocate a buffer for headers and payload
        uint8_t hdr[HEADER_SIZE];

        while (g_running.load() && conn_ok) {
            // read header
            if (!recv_all(cli_fd, hdr, HEADER_SIZE)) {
                std::cout << "[TCP] header recv failed or connection closed\n";
                break;
            }
            // parse header (little-endian)
            uint32_t magic = (uint32_t)hdr[0] | ((uint32_t)hdr[1] << 8) | ((uint32_t)hdr[2] << 16) | ((uint32_t)hdr[3] << 24);
            if (magic != HEADER_MAGIC) {
                std::cerr << "[TCP] bad magic: " << std::hex << magic << std::dec << std::endl;
                conn_ok = false;
                break;
            }
            uint32_t seq = (uint32_t)hdr[4] | ((uint32_t)hdr[5] << 8) | ((uint32_t)hdr[6] << 16) | ((uint32_t)hdr[7] << 24);
            uint64_t first_sample_index = 0;
            for (int i = 0; i < 8; ++i) first_sample_index |= ((uint64_t)hdr[8 + i]) << (8 * i);
            uint64_t timestamp_us = 0;
            for (int i = 0; i < 8; ++i) timestamp_us |= ((uint64_t)hdr[16 + i]) << (8 * i);
            uint16_t frames = (uint16_t)hdr[24] | ((uint16_t)hdr[25] << 8);
            uint8_t channels = hdr[26];
            uint8_t bytes_per_sample = hdr[27];
            uint32_t sample_rate = (uint32_t)hdr[28] | ((uint32_t)hdr[29] << 8) | ((uint32_t)hdr[30] << 16) | ((uint32_t)hdr[31] << 24);
            uint16_t format_id = (uint16_t)hdr[32] | ((uint16_t)hdr[33] << 8);

            // validate basic fields (warn, not fatal)
            if (sample_rate != SAMPLE_RATE) {
                std::cerr << "[TCP] warning sample_rate mismatch: " << sample_rate << " != " << SAMPLE_RATE << std::endl;
            }
            if (channels != CHANNELS) {
                std::cerr << "[TCP] warning channels mismatch: " << int(channels) << " != " << int(CHANNELS) << std::endl;
            }
            if (bytes_per_sample != IN_BYTES_PER_SAMPLE) {
                std::cerr << "[TCP] warning bytes_per_sample mismatch: " << int(bytes_per_sample) << " != " << int(IN_BYTES_PER_SAMPLE) << std::endl;
            }
            if (format_id != FORMAT_INT32_LEFT24) {
                std::cerr << "[TCP] warning format_id mismatch: " << format_id << std::endl;
            }

            size_t payload_bytes = (size_t)frames * (size_t)channels * (size_t)bytes_per_sample;
            if (payload_bytes == 0) {
                std::cerr << "[TCP] zero payload\n";
                continue;
            }

            // Read payload into buffer (allocate per-packet to conserve memory on tiny devices)
            std::vector<uint8_t> payload(payload_bytes);
            if (!recv_all(cli_fd, payload.data(), payload_bytes)) {
                std::cerr << "[TCP] payload recv failed\n";
                conn_ok = false;
                break;
            }

            // Convert and write to WAV: for each frame, convert int32->24bit (3 bytes), apply gain.
            // We'll multiply int32 value by gain then shift; keep in int64 to avoid overflow.
            std::vector<uint8_t> out_bytes;
            out_bytes.reserve((size_t)frames * OUT_BYTES_PER_SAMPLE);
            for (size_t i = 0; i < (size_t)frames; ++i) {
                // read int32 little-endian
                size_t base = i * 4;
                int32_t s32 = (int32_t)(
                    (uint32_t)payload[base + 0]
                    | ((uint32_t)payload[base + 1] << 8)
                    | ((uint32_t)payload[base + 2] << 16)
                    | ((uint32_t)payload[base + 3] << 24));
                // apply gain in floating then convert back
                double gf = g_gain.load();
                double scaled = (double)s32 * gf;
                // clamp to int32 range
                if (scaled > (double)INT32_MAX) scaled = (double)INT32_MAX;
                if (scaled < (double)INT32_MIN) scaled = (double)INT32_MIN;
                int32_t s32_scaled = (int32_t)std::llround(scaled);
                // align to 24-bit
                int32_t s24 = s32_scaled >> 8; // arithmetic shift
                uint32_t u24 = (uint32_t)s24 & 0xFFFFFFu;
                out_bytes.push_back((uint8_t)(u24 & 0xFF));
                out_bytes.push_back((uint8_t)((u24 >> 8) & 0xFF));
                out_bytes.push_back((uint8_t)((u24 >> 16) & 0xFF));
            }

            // write bytes to file (protected by mutex)
            {
                std::lock_guard<std::mutex> lk(g_file_mutex);
                // append
                size_t wrote = fwrite(out_bytes.data(), 1, out_bytes.size(), out);
                if (wrote != out_bytes.size()) {
                    std::cerr << "[WAV] short write: " << wrote << " of " << out_bytes.size() << std::endl;
                }
                fflush(out);
                g_total_samples_written += (int)frames;
            }

            // update status
            g_highest_received_index.store(first_sample_index + frames - 1);
            g_last_seq.store(seq);
        } // while receiving packets

        // close connection
        close(cli_fd);

        // finalize file header now (or leave for later). We'll finalize on exit, not each disconnect.
        std::cout << "[TCP] client disconnected, continuing listen" << std::endl;
        if (out) {
            std::lock_guard<std::mutex> lk(g_file_mutex);
            // leave header placeholder until program exit to minimize header rewrites
            fclose(out);
            out = nullptr;
        }
    } // accept loop

    close(listen_fd);
    std::cout << "[TCP] server exiting\n";
}

// ----------------- Simple HTTP server (very small) -----------------
// Supports:
//   GET /            -> returns a tiny HTML control page
//   GET /status      -> plain text status
//   GET /control?gain=1.5  -> set gain (simple)
void http_server_loop() {
    int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0) { perror("http socket"); return; }
    int reuse = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    struct sockaddr_in addr;
    addr.sin_family = AF_INET;
    addr.sin_port = htons(HTTP_PORT);
    addr.sin_addr.s_addr = INADDR_ANY;
    if (bind(listen_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("http bind");
        close(listen_fd);
        return;
    }
    if (listen(listen_fd, 4) < 0) {
        perror("http listen");
        close(listen_fd);
        return;
    }
    std::cout << "[HTTP] control server listening on :" << HTTP_PORT << std::endl;

    const char html_page[] =
        "<!doctype html><html><head><meta charset='utf-8'><title>ESP Receiver</title></head>"
        "<body><h2>ESP Receiver Control</h2>"
        "<form action='/control' method='get'>Gain: <input name='gain' value='1.0'/> <input type='submit'/></form>"
        "<p>GET /status for status</p>"
        "</body></html>";

    while (g_running.load()) {
        struct sockaddr_in cli;
        socklen_t cli_len = sizeof(cli);
        int cli_fd = accept(listen_fd, (struct sockaddr*)&cli, &cli_len);
        if (cli_fd < 0) {
            if (errno == EINTR) continue;
            perror("http accept");
            break;
        }

        // read a small request (we keep it simple)
        char buf[2048];
        ssize_t r = recv(cli_fd, buf, sizeof(buf)-1, 0);
        if (r <= 0) {
            close(cli_fd);
            continue;
        }
        buf[r] = 0;
        std::string req(buf);
        // parse first line
        std::istringstream reqs(req);
        std::string method, path;
        reqs >> method >> path;
        if (method != "GET") {
            const char* resp = "HTTP/1.1 405 Method Not Allowed\r\nContent-Length:0\r\n\r\n";
            send(cli_fd, resp, strlen(resp), 0);
            close(cli_fd);
            continue;
        }

        if (path == "/" || path == "/index.html") {
            std::ostringstream resp;
            resp << "HTTP/1.1 200 OK\r\nContent-Type: text/html\r\nContent-Length: " << strlen(html_page) << "\r\n\r\n" << html_page;
            std::string s = resp.str();
            send(cli_fd, s.c_str(), s.size(), 0);
            close(cli_fd);
            continue;
        } else if (path.rfind("/status",0) == 0) {
            std::ostringstream body;
            body << "running=" << (g_running.load()? "1":"0") << "\n";
            body << "gain=" << g_gain.load() << "\n";
            body << "last_seq=" << g_last_seq.load() << "\n";
            body << "highest_sample_index=" << g_highest_received_index.load() << "\n";
            body << "samples_written=" << g_total_samples_written << "\n";
            std::string b = body.str();
            std::ostringstream resp;
            resp << "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nContent-Length: " << b.size() << "\r\n\r\n" << b;
            std::string s = resp.str();
            send(cli_fd, s.c_str(), s.size(), 0);
            close(cli_fd);
            continue;
        } else if (path.rfind("/control",0) == 0) {
            // simple parsing for ?gain=...
            size_t qpos = path.find('?');
            if (qpos != std::string::npos) {
                std::string qs = path.substr(qpos + 1);
                // parse key=value pairs
                std::istringstream qss(qs);
                std::string token;
                while (std::getline(qss, token, '&')) {
                    size_t eq = token.find('=');
                    if (eq != std::string::npos) {
                        std::string k = token.substr(0, eq);
                        std::string v = token.substr(eq + 1);
                        if (k == "gain") {
                            try {
                                double g = std::stod(v);
                                g_gain.store(g);
                            } catch (...) { /* ignore parse error */ }
                        }
                    }
                }
            }
            const char* redirect = "HTTP/1.1 302 Found\r\nLocation: /\r\nContent-Length:0\r\n\r\n";
            send(cli_fd, redirect, strlen(redirect), 0);
            close(cli_fd);
            continue;
        } else {
            const char* notf = "HTTP/1.1 404 Not Found\r\nContent-Length:0\r\n\r\n";
            send(cli_fd, notf, strlen(notf), 0);
            close(cli_fd);
            continue;
        }
    }

    close(listen_fd);
    std::cout << "[HTTP] server exiting\n";
}

// ----------------- signal handler -----------------
void sigint_handler(int) {
    std::cout << "\nSIGINT received, shutting down...\n";
    g_running.store(false);
}

// ----------------- main -----------------
int main(int argc, char** argv) {
    (void)argc; (void)argv;
    signal(SIGINT, sigint_handler);

    std::thread tcp_thread(tcp_server_loop);
    std::thread http_thread(http_server_loop);

    std::cout << "Receiver running. HTTP control on port " << HTTP_PORT << ". Press Ctrl-C to stop.\n";

    // Wait until stop
    while (g_running.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }

    // finalize: patch WAV header if file exists
    {
        std::lock_guard<std::mutex> lk(g_file_mutex);
        // open file for read+update
        FILE* f = fopen(OUT_FILENAME, "rb+");
        if (f) {
            finalize_wav_header(f, (uint32_t)g_total_samples_written, CHANNELS, SAMPLE_RATE);
            fclose(f);
            std::cout << "[WAV] finalized header, samples_written=" << g_total_samples_written << std::endl;
        } else {
            std::cerr << "[WAV] could not open " << OUT_FILENAME << " to finalize header\n";
        }
    }

    if (tcp_thread.joinable()) tcp_thread.join();
    if (http_thread.joinable()) http_thread.join();

    std::cout << "Exited.\n";
    return 0;
}
