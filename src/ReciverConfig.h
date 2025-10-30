#pragma once
#include <Arduino.h>
#include <Preferences.h>
#include <mutex>
#include "audio_read_send.h"

// Use the same PREF_NAMESPACE string you use elsewhere.
// If you already define PREF_NAMESPACE elsewhere, this #ifndef won't override it.
#ifndef PREF_NAMESPACE
 #define PREF_NAMESPACE "config"
#endif

class ReciverConfig {
private:
    Preferences prefs_;
    std::mutex mu_;
    IPAddress ip_{0,0,0,0};
    uint16_t port_{0};

public:
    ReciverConfig() = default;

    // We no longer call prefs_.end() in destructor because each method calls end().
    ~ReciverConfig() = default;

    // Convenience: load stored values from NVM (Preferences)
    void begin()
    {
        // simply call load() — load() handles prefs begin/end itself
        load();
    }

    // load current values from Preferences (thread-safe)
    void load()
    {
        std::lock_guard<std::mutex> lock(mu_);
        prefs_.begin(PREF_NAMESPACE, true); // read-only
        String saved_ip = prefs_.getString("pc_ip", "");
        String saved_port = prefs_.getString("pc_port", "");
        prefs_.end();

        IPAddress tmp;
        if (saved_ip.length() && tmp.fromString(saved_ip)) {
            ip_ = tmp;
        } else {
            ip_ = IPAddress(0,0,0,0);
        }

        if (saved_port.length()) {
            long p = saved_port.toInt();
            port_ = (p > 0 && p <= 65535) ? static_cast<uint16_t>(p) : 0;
        } else {
            port_ = 0;
        }
    }

    // save values to Preferences (thread-safe)
    void save(const char* ip_str, uint16_t port)
    {
        std::lock_guard<std::mutex> lock(mu_);
        prefs_.begin(PREF_NAMESPACE, false); // read-write
        prefs_.putString("pc_ip", String(ip_str));
        prefs_.putString("pc_port", String((unsigned)port));
        prefs_.end();

        // update in-memory copy if valid
        IPAddress tmp;
        if (ip_str && ip_str[0] && tmp.fromString(String(ip_str))) {
            ip_ = tmp;
            port_ = port;
        }
    }

    // clear stored config
    void clear()
    {
        std::lock_guard<std::mutex> lock(mu_);
        prefs_.begin(PREF_NAMESPACE, false);
        prefs_.remove("pc_ip");
        prefs_.remove("pc_port");
        prefs_.end();

        ip_ = IPAddress(0,0,0,0);
        port_ = 0;
    }

    // get current values (thread-safe copy)
    void get(IPAddress &out_ip, uint16_t &out_port)
    {
        std::lock_guard<std::mutex> lock(mu_);
        out_ip = ip_;
        out_port = port_;
    }

    // convenience accessors
    String ipString()
    {
        std::lock_guard<std::mutex> lock(mu_);
        if (ip_ == IPAddress(0,0,0,0)) return String("(none)");
        return ip_.toString();
    }

    unsigned short port()
    {
        std::lock_guard<std::mutex> lock(mu_);
        return port_;
    }

    bool isvalid()
    {
        std::lock_guard<std::mutex> lock(mu_);
        return (ip_ != IPAddress(0,0,0,0) && port_ != 0);
    }
};
