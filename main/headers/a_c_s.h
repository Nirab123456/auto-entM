#pragma once
#include <Arduino.h>

inline constexpr uint8_t NUMBERS_OF_CHANNELS = 1;
inline constexpr uint8_t BYTES_PER_SAMPLE   = 4;
// inline constexpr size_t BYTES_TO_READ       = ((size_t)(FRAMES_PER_PACKET * BYTES_PER_SAMPLE * 2));
// inline constexpr size_t PAYLOAD_BYTES = ((size_t)FRAMES_PER_PACKET * BYTES_PER_SAMPLE * NUMBERS_OF_CHANNELS);
// inline constexpr size_t NEEDED_WORDS = ((size_t)FRAMES_PER_PACKET*2);
inline constexpr uint8_t DEFAULT_CHANNEL_COUNT = 2;

inline constexpr const char* PREF_NAMESPACE = "config";
constexpr const char* DEFAULT_AP_NAME = "auto_antmAP";
constexpr const char* PREFS_IP_ID = "pc_ip";
constexpr const char* PREFS_IP_LABEL = "Receiver_IP";
constexpr const char* PREFS_PORT_ID= "pc_port";
constexpr const char* PREFS_PORT_LABEL= "Receiver_Port";

inline constexpr uint16_t DEFAULT_STOP_TASK_WAIT = 500;
inline constexpr uint8_t MIN_BYTES_READ = 4;
inline constexpr uint8_t SIZE_OF_A_BYTE_IN_BITS = 8;
constexpr char* Default_AP_PASS = nullptr;
constexpr uint8_t DEFAULT_IP_BUFFER_SIZE = 40;
constexpr uint8_t DEFAULT_PORT_BUFFER_SIZE = 16;
constexpr size_t ERR_SZ = 128;
