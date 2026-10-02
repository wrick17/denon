#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <sys/types.h>
#include <esp_attr.h>
#include <esp_rom_sys.h>
#include <esp_system.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/portmacro.h>
#include <esp32-hal-uart.h>

// Included by main.cpp only. Keep console output intact; the three independent
// paths are Arduino Serial, newlib stdout/stderr, and the ROM printf channel.
namespace networkLogs {
constexpr unsigned kCapacity = 32;
constexpr unsigned kChunkBytes = 128;
constexpr unsigned kMaxPage = 16;
struct Record {
  uint32_t seq;
  uint32_t uptimeMs;
  uint16_t length;
  bool continued;
  char text[kChunkBytes + 1];
};
struct Stats {
  uint32_t oldestSeq;
  uint32_t nextSeq;
  uint32_t dropped;
};

DRAM_ATTR Record records[kCapacity] = {};
DRAM_ATTR portMUX_TYPE mutex = portMUX_INITIALIZER_UNLOCKED;
DRAM_ATTR std::atomic<bool> enabled{false};
DRAM_ATTR unsigned first = 0;
DRAM_ATTR unsigned count = 0;
DRAM_ATTR uint32_t nextSequence = 1;
DRAM_ATTR uint32_t overwritten = 0;
DRAM_ATTR char romPending[kChunkBytes] = {};
DRAM_ATTR unsigned romLength = 0;
DRAM_ATTR uint32_t romStartedAt = 0;
char currentBootId[17] = {};

// ESP32 esp_timer_get_time and its constant-division helper reside in IRAM/ROM.
// This path is also used by the ROM console while flash cache is unavailable.
uint32_t IRAM_ATTR uptimeMs() {
  return static_cast<uint32_t>(esp_timer_get_time() / 1000);
}

// Called with mutex held. At most 128 bytes are copied; no logging, allocation,
// UART access, socket access, or formatting is allowed in this critical section.
void IRAM_ATTR appendLocked(const char *text, unsigned length, uint32_t at) {
  if (count == kCapacity) {
    first = (first + 1) % kCapacity;
    --count;
    ++overwritten;
  }
  Record &record = records[(first + count) % kCapacity];
  record.seq = nextSequence++;
  record.uptimeMs = at;
  record.length = static_cast<uint16_t>(length);
  record.continued = text[length - 1] != '\n';
  for (unsigned i = 0; i < length; ++i) record.text[i] = text[i];
  record.text[length] = '\0';
  ++count;
}

void append(const void *data, size_t length) {
  if (!enabled.load(std::memory_order_relaxed) || !data) return;
  const char *text = static_cast<const char *>(data);
  const uint32_t at = uptimeMs();
  while (length) {
    const unsigned chunk = length > kChunkBytes ? kChunkBytes : length;
    portENTER_CRITICAL_SAFE(&mutex);
    appendLocked(text, chunk, at);
    portEXIT_CRITICAL_SAFE(&mutex);
    text += chunk;
    length -= chunk;
  }
}

void IRAM_ATTR romPutc(char c) {
  if (!enabled.load(std::memory_order_relaxed)) return;
  const uint32_t at = uptimeMs();
  portENTER_CRITICAL_SAFE(&mutex);
  if (!romLength) romStartedAt = at;
  romPending[romLength++] = c;
  if (c == '\n' || romLength == kChunkBytes) {
    appendLocked(romPending, romLength, romStartedAt);
    romLength = 0;
  }
  portEXIT_CRITICAL_SAFE(&mutex);
}

void begin() {
  // No NVS writes: the server separates sequence numbers using this boot ID.
  snprintf(currentBootId, sizeof(currentBootId), "%08lx%08lx",
           static_cast<unsigned long>(esp_random()),
           static_cast<unsigned long>(esp_random()));
  enabled.store(true, std::memory_order_relaxed);
  // Arduino can clear channel 2 when configuring Serial debug output. Install
  // after Serial.begin; this application does not reconfigure/end Serial later.
  esp_rom_install_channel_putc(2, romPutc);
}

const char *bootId() { return currentBootId; }

void flush() {
  portENTER_CRITICAL_SAFE(&mutex);
  if (romLength) {
    appendLocked(romPending, romLength, romStartedAt);
    romLength = 0;
  }
  portEXIT_CRITICAL_SAFE(&mutex);
}

Stats stats() {
  portENTER_CRITICAL_SAFE(&mutex);
  const Stats result{count ? records[first].seq : nextSequence,
                     nextSequence, overwritten};
  portEXIT_CRITICAL_SAFE(&mutex);
  return result;
}

bool readAfter(uint32_t after, Record &out) {
  bool found = false;
  portENTER_CRITICAL_SAFE(&mutex);
  for (unsigned i = 0; i < count; ++i) {
    const Record &record = records[(first + i) % kCapacity];
    if (record.seq > after) {
      out = record;
      found = true;
      break;
    }
  }
  portEXIT_CRITICAL_SAFE(&mutex);
  return found;
}
}  // namespace networkLogs

extern "C" void __real_uartWrite(uart_t *, uint8_t);
extern "C" void __real_uartWriteBuf(uart_t *, const uint8_t *, size_t);
extern "C" void __wrap_uartWrite(uart_t *uart, uint8_t c) {
  if (uart) networkLogs::append(&c, 1);
  __real_uartWrite(uart, c);
}
extern "C" void __wrap_uartWriteBuf(uart_t *uart, const uint8_t *data, size_t size) {
  if (uart) networkLogs::append(data, size);
  __real_uartWriteBuf(uart, data, size);
}

// The installed IDF aliases _write_r to esp_vfs_write. Wrap both external names
// so normal ESP_LOG/vprintf and direct VFS calls are captured. Forward directly
// to the original backend; it never calls Arduino's uartWrite/uartWriteBuf.
struct _reent;
extern "C" ssize_t __real__write_r(struct _reent *, int, const void *, size_t);
extern "C" ssize_t __real_esp_vfs_write(struct _reent *, int, const void *, size_t);
extern "C" ssize_t __wrap__write_r(struct _reent *r, int fd,
                                  const void *data, size_t size) {
  if (fd == 1 || fd == 2) networkLogs::append(data, size);
  return __real__write_r(r, fd, data, size);
}
extern "C" ssize_t __wrap_esp_vfs_write(struct _reent *r, int fd,
                                       const void *data, size_t size) {
  if (fd == 1 || fd == 2) networkLogs::append(data, size);
  return __real_esp_vfs_write(r, fd, data, size);
}
