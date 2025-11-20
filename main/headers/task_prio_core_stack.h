#pragma once
#include <stdlib.h>
#include <stdint.h>

constexpr size_t DEFAULT_STACK_I2SReadTask = 4096;
constexpr size_t DEFAULT_STACK_RingWriterTask = 8192;
constexpr size_t DEFAULT_STACK_NetworkTask = 8192;
constexpr size_t DEFAULT_STACK_NetworkDataWriterTask = 4096;

constexpr uint8_t DEFAULT_CORE_I2SReadTask = 1;
constexpr uint8_t DEFAULT_CORE_RingWriterTask = 1;
constexpr uint8_t DEFAULT_CORE_NetworkTask = 1;
constexpr uint8_t DEFAULT_CORE_NetworkDataWriterTask = 1;

constexpr uint8_t DEFAULT_PRIO_I2SReadTask = 3;
constexpr uint8_t DEFAULT_PRIO_RingWriterTask = 2;
constexpr uint8_t DEFAULT_PRIO_NetworkTask = 2;
constexpr uint8_t DEFAULT_PRIO_NetworkDataWriterTask = 3;
