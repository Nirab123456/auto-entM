#pragma once
#include <Arduino.h>
#include <Preferences.h>
#include <mutex>
#include "a_c_s.h"



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

};