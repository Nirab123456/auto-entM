#pragma once
#include "audio_read_send.h"
#include <Preferences.h>
#include <mutex>
#include "a_c_s.h"
#include <algorithm> 
#include <WiFiManager.h>
#include "nvs_flash.h"

inline constexpr uint16_t CONNECTION_RETRY_INTERVAL_MS = 200;


class ReciverConfig {
    private:
        const char* prefs_namespace_;
        uint8_t prefs_rst_open_portal_pin_ = 0xff;
        TickType_t debounce_ticks_ = pdMS_TO_TICKS(20);
        uint32_t hold_ms_ = 800;
        static void IRAM_ATTR confButtonIsrHandle();
        void ConfButtonTaskLoop();
        bool GSVIpPort(
            char* ip_buffer,
            char* port_buffer,
            bool force_start_conf_portal = false,
            const char* ap_ssid = nullptr,
            const char* ap_password = nullptr,
            uint8_t ip_buffer_len = DEFAULT_IP_BUFFER_SIZE,
            uint8_t port_buffer_len = DEFAULT_PORT_BUFFER_SIZE
        );

        std::function<void()> startConfigPortalCb_;
        std::atomic<bool> stopping_{false};

        void StopAndClean();

        Preferences prefs_;
        std::mutex mu_;
        IPAddress ip_{0,0,0,0};
        uint16_t port_{0};

        AUDIO_RS*   audio_rs_class_ptr_{nullptr};

        ReciverConfig(const ReciverConfig&) = delete;
        ReciverConfig& operator = (const ReciverConfig&) = delete;
    public:
        ReciverConfig(const char* prefs_namespace = "config");
        ~ReciverConfig();
        static TaskHandle_t ConfSRButtonTaskHandle_;

        void load();
        void save(const char* ip_str, uint16_t port);
        void clear();
        void get(IPAddress &out_ip, uint16_t &out_port);
        String ipString();
        unsigned short port();
        bool isValid();
        void begin();
        bool ConnectTOReciverIP(WiFiClient* WiFi_TCPClient);
        bool TCPWriteAll(WiFiClient* client, const uint8_t* data, size_t len,
                        uint32_t timeout_ms = 2000, int max_retries = 3,
                        size_t chunk_size = 1400);
        
        bool AttachResetButton(
            uint8_t button_pin,
            TickType_t debounce_ms = 20,
            uint32_t hold_ms = 800,
            UBaseType_t task_prio = 5,
            uint32_t task_stack_bytes = 3072,
            BaseType_t core = -99,
            void* arg = nullptr
        );
        
        void setStartConfPortalCallback(std::function<void()> cb);

        void ClearPrefs();

        void DetachResetButton(TickType_t wait_ms = pdMS_TO_TICKS(500));

        static void ConfRstButtonTrampoline(void* pv);
        static void StartConfPortalTrampoline(void* pv);

        void setAudioRsPtr(AUDIO_RS* p);
        bool StartConfigPortal(
            bool force_start_conf_portal = false,
            const char* ap_ssid = DEFAULT_AP_NAME, 
            const char* ap_password = Default_AP_PASS
        );

        std::function<bool(/*if needed set parameters*/)> start_task_fn_ptr{nullptr};

};