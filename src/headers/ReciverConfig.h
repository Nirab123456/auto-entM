#pragma once
#include <Arduino.h>
#include <Preferences.h>
#include <mutex>
#include "a_c_s.h"
#include <WiFi.h>

inline constexpr uint16_t CONNECTION_RETRY_INTERVAL_MS = 200;


class ReciverConfig {
    private:
        Preferences prefs_;
        std::mutex mu_;
        IPAddress ip_{0,0,0,0};
        uint16_t port_{0};
    public:
        ReciverConfig() = default;
        ~ReciverConfig() = default;
        void load();
        void save(const char* ip_str, uint16_t port);
        void clear();
        void get(IPAddress &out_ip, uint16_t &out_port);
        String ipString();
        unsigned short port();
        bool isValid();
        void begin();
        bool ConnectTOReciverIP(WiFiClient* WiFi_TCPClient);
        bool tcpWriteAll(Client* client, const uint8_t* data, size_t len,
                        uint32_t timeout_ms = 2000, int max_retries = 3,
                        size_t chunk_size = 1400);
};