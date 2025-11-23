#pragma once 
#include <span>
#include <atomic>
#include <memory>
#include <cstdint>
#include <functional>
#include <vector>
#include <WiFi.h>            // for WiFiClient and WiFi.isConnected()
#include "microphoneConfig.h"
#include "nvs_flash.h"
#include "psramalloc.h"

using TASK_TRAMPOLINE_FN = void(*)(void*);

// forward declare your ReciverConfig (you provided this elsewhere)
class ReciverConfig;

class AUDIO_RS 
{
    public:
        enum class OverRunPolicy : uint8_t {
            DROP_NEWEST = 0,
            DROP_OLDEST = 1
        };
        
    private:
        // -----------------------
        // core shared atomics
        // -----------------------
        std::atomic<bool>i2s_installed_{false};

        const uint32_t HEADER_MAGIC_ = 0x45535032;
        const uint8_t FORMAT_INT32_LEFT24_ = 1;

        std::shared_ptr<std::atomic<bool>>          consumer_ready_sp_{nullptr};
        std::shared_ptr<std::atomic<size_t>>        ring_head_sp_{nullptr};
        std::shared_ptr<std::atomic<size_t>>        ring_tail_sp_{nullptr};
        std::shared_ptr<std::atomic<uint64_t>>      abs_idx_sp_{nullptr};
        std::shared_ptr<std::atomic<uint32_t>>      sequence_counter_{nullptr};
        uint8_t CHANNEL_COUNT_{0};
        std::atomic<uint32_t> connection_failure_{0};

        uint16_t conn_retry_base_ms_ = 2000;
        uint16_t conn_retry_max_ms_ = 30000;



        // overrun policy & stats
        OverRunPolicy  overrun_policy_ = OverRunPolicy::DROP_NEWEST;
        std::atomic<uint32_t> drop_count_newest_{0};
        std::atomic<uint32_t> drop_count_oldest_{0};

        // -----------------------
        // non-owning buffers (spans)
        // ring_payload_flat_ is a flat view of RING_SIZE * frames_per_packet_
        // -----------------------
        std::span<uint32_t> i2s_buffer_{};
        std::span<uint32_t> ring_payload_flat_{};
        size_t frames_per_packet_{0};

        // per-slot metadata (non-owning spans pointing at arrays you must provide)
        std::span<uint16_t> ring_frames_span_{};        // length == ring_slots
        std::span<uint64_t> ring_first_index_span_{};
        std::span<uint64_t> ring_timestamp_span_{};

        // -----------------------
        // RTOS / task primitives
        // -----------------------
        QueueHandle_t       i2s_queue_{nullptr};
        TaskHandle_t        read_task_{nullptr};
        TaskHandle_t        write_task_{nullptr};
        TaskHandle_t        fingerprint_task_{nullptr};
        TaskHandle_t        networktask_{nullptr};

        // -----------------------
        // Networking / ReciverConfig integration (dependency injection)
        // -----------------------
        ReciverConfig*                              recfg_ptr_{nullptr}; // optional
        std::function<bool(IPAddress&, uint16_t&)>  recfg_getter_{nullptr}; // optional getter

        // TCP client or callbacks (choose either approach or supply both)
        WiFiClient*                                  WiFi_tcp_client_ptr_{nullptr}; // optional
        std::function<bool(IPAddress, uint16_t)>    tcp_connect_fn_{nullptr}; // return true on success
        std::function<int(const uint8_t*, size_t)>  tcp_write_fn_{nullptr};   // return bytes written
        std::function<void()>                       tcp_client_stop_fn_{nullptr};
        std::function<bool()>                       tcp_client_connected_fn_{nullptr}; // returns true if connected

        // header buffer and writer callback
        std::vector<uint8_t>                        header_buffer_;
        std::atomic<size_t>                         header_size_{0};
        std::mutex          header_mu_;
        std::function<void(uint32_t, uint64_t, uint64_t, uint16_t)> write_tcp_header_fn_{nullptr};

        // network slot queue and writer task handle
        QueueHandle_t network_slot_queue_{nullptr};    // carries size_t slot indices
        TaskHandle_t  network_writer_task_{nullptr};

        //i2s channel handle
        i2s_chan_handle_t rx_chan_ = nullptr;
        i2s_chan_handle_t tx_chan_ = nullptr;

        std::atomic<bool> stopping_{false};

        // queue length: choose based on expected backlog (e.g. network latency / ring size)
        static constexpr size_t NETWORK_SLOT_QUEUE_LEN = 16;

        MicrophoneConfig micfg_;
        std::atomic<bool> mic_configured_{false};

        // ring clear / reset callback (optional)
        // std::function<void()>                        clear_ring_and_reset_indices_fn_{nullptr};

        // -----------------------
        // Internal task loops (instance methods)
        // -----------------------
        void I2SReaderLoop();
        void RingWriterLoop();
        void FingerPrintLoop();
        void NetworkTaskLoop();
        void NetworkDataWriterLoop();
        void ClearHandleField(TaskHandle_t h);
        bool IsKnownHandle(TaskHandle_t h) const;
        bool stopping_check_del(char* taskname);
        void WriteTCPHeader(uint32_t seq, uint64_t first_sample_index, uint64_t timestamp_us, uint16_t number_of_frames);

        bool BasicNecesseryChecksLoop(char* taskname);
    public:

        TaskHandle_t i2s_reader_handle_ = nullptr;
        TaskHandle_t ring_writer_handle_ = nullptr;
        TaskHandle_t network_handle_ = nullptr;
        TaskHandle_t network_writer_handle_ = nullptr;
        TaskHandle_t monitor_handle_ = nullptr;
        TaskHandle_t conf_portal_rst_button_handler_ = nullptr;
        
        AUDIO_RS() = default;

        AUDIO_RS(
            std::span<uint32_t> i2s_buffer,
            std::span<uint32_t> ring_payload_flat,
            size_t frames_per_packet,
            std::shared_ptr<std::atomic<bool>> consumer_ready = nullptr,
            std::shared_ptr<std::atomic<size_t>> ring_head = nullptr,
            std::shared_ptr<std::atomic<size_t>> ring_tail = nullptr,
            std::shared_ptr<std::atomic<uint64_t>> abs_idx = nullptr
        );
        bool initI2S();
        void deinitI2S();
        void Ring_clear_Rst();

        void PauseNetworkStreaming();
        bool ReqNetworkReconnect();
        void set_consumer_ready_flag(bool v);
        std::shared_ptr<std::atomic<bool>>Get_Consumer_Ready_ptr() const;
        bool Has_Consumer_Ready() const;

        // -----------------------
        // basic setters for atomics & buffers
        // -----------------------
        void set_consumer_ready(std::shared_ptr<std::atomic<bool>> ar);
        void set_ring_head(std::shared_ptr<std::atomic<size_t>> ar);
        void set_ring_tail(std::shared_ptr<std::atomic<size_t>> ar);
        void set_abs_idx(std::shared_ptr<std::atomic<uint64_t>> ar);
        void set_sequence_counter(std::shared_ptr<std::atomic<uint32_t>> seq);

        void set_i2s_buffer(std::span<uint32_t> i2s_buffer);
        void set_ring_payload_flat(std::span<uint32_t> flat, size_t frames_per_packet);

        // metadata spans (non-owning)
        void set_ring_metadata_spans(std::span<uint16_t> frames_span,
                                     std::span<uint64_t> first_index_span,
                                     std::span<uint64_t> timestamp_span);

        // -----------------------
        // networking setters (dependency injection)
        // -----------------------
        // Prefer passing either ReciverConfig* or a getter lambda. If both provided,
        // recfg_getter_ will be used first.
        void set_reciver_config_ptr(ReciverConfig* ptr);
        void set_reciver_config_getter(std::function<bool(IPAddress&, uint16_t&)> getter);

        // TCP client pointer (WiFiClient), or alternatively set the connect/write callbacks
        void set_WiFi_client_ptr(WiFiClient* client);
        void set_tcp_connect_fn(std::function<bool(IPAddress, uint16_t)> connect_fn);
        void set_tcp_write_fn(std::function<int(const uint8_t*, size_t)> write_fn);
        void set_tcp_client_stop_fn(std::function<void()> stop_fn);
        void set_tcp_client_connected_fn(std::function<bool()> connected_fn);

        // header buffer and header writer callback
        void set_header_buffer_size(size_t n = 0);
        void set_write_tcp_header_fn(std::function<void(uint32_t, uint64_t, uint64_t, uint16_t)> fn);

        // clear/reset callback
        void set_clear_ring_and_reset_indices_fn(std::function<void()> fn);

        void set_micfg(const MicrophoneConfig &cfg);
        bool set_network_slot_queue(QueueHandle_t q);

        // -----------------------
        // task/trampoline helpers
        // -----------------------
        static void I2SReadTrampoline(void* pv);
        static void RingWriterFRMI2STrampoline(void* pv);
        static void NetworkTaskLoopTrampoline(void* pv);
        static void NetworkDataWriterLoopTrampoline(void* pv);

        // start modular tasks (you already have similar; kept signature)
        bool start_task(
            const char* name,
            uint32_t stack,
            UBaseType_t prio,
            BaseType_t core,
            TASK_TRAMPOLINE_FN trampoline,
            void* arg,
            TaskHandle_t* out_handle
        );

        void stop_task(
            TaskHandle_t handle,
            TickType_t wait_ms
        );

        // convenience overrun setters
        void set_overrun_policy(OverRunPolicy p) { overrun_policy_ = p; }
        void ovverrunpolicy_newest() { set_overrun_policy(OverRunPolicy::DROP_NEWEST); }
        void ovverrunpolicy_oldest() { set_overrun_policy(OverRunPolicy::DROP_OLDEST); }

        // read-only stats
        uint32_t get_drop_count_newest() const { return drop_count_newest_.load(std::memory_order_relaxed); }
        uint32_t get_drop_count_oldest() const { return drop_count_oldest_.load(std::memory_order_relaxed); }
        size_t get_frames_per_packet() const { return frames_per_packet_; }

        // expose some spans for read-only inspection if needed
        std::span<uint32_t> get_i2s_buffer() const { return i2s_buffer_; }
        std::span<uint32_t> get_ring_payload_flat() const { return ring_payload_flat_; }
        void nvsInitMain();
};
