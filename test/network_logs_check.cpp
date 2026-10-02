// Compile the production capture hooks; stub only their hardware/OS boundaries.
#include <cassert>
#include <cerrno>
#include <cstring>
#include <string>
#include <thread>
#include <vector>
#include "network_logs.h"

thread_local unsigned testCriticalDepth = 0;
std::atomic<uint64_t> clockUs{0};
std::atomic<unsigned> forwarded{0};
void (*romConsole)(char) = nullptr;
uart_t testUart;

int64_t esp_timer_get_time() {
  assert(testCriticalDepth == 0);
  return clockUs.fetch_add(1000);
}
uint32_t esp_random() {
  static uint32_t value = 0;
  return ++value;
}
void esp_rom_install_channel_putc(int channel, void (*callback)(char)) {
  assert(channel == 2 && testCriticalDepth == 0);
  romConsole = callback;
}
extern "C" void __real_uartWrite(uart_t *, uint8_t) {
  assert(testCriticalDepth == 0);
  ++forwarded;
}
extern "C" void __real_uartWriteBuf(uart_t *, const uint8_t *, size_t) {
  assert(testCriticalDepth == 0);
  ++forwarded;
}
extern "C" ssize_t __real__write_r(struct _reent *, int, const void *, size_t size) {
  assert(testCriticalDepth == 0);
  ++forwarded;
  return static_cast<ssize_t>(size);
}
extern "C" ssize_t __real_esp_vfs_write(struct _reent *, int fd,
                                       const void *, size_t size) {
  assert(testCriticalDepth == 0);
  ++forwarded;
  if (fd == 99) { errno = EBADF; return -1; }
  return static_cast<ssize_t>(size);
}

void resetCapture() {
  networkLogs::enabled.store(false);
  networkLogs::first = networkLogs::count = networkLogs::romLength = 0;
  networkLogs::nextSequence = 1;
  networkLogs::overwritten = 0;
  networkLogs::begin();
}
std::vector<networkLogs::Record> readAll() {
  networkLogs::flush();
  std::vector<networkLogs::Record> result;
  networkLogs::Record record;
  uint32_t after = 0;
  while (networkLogs::readAfter(after, record)) {
    result.push_back(record);
    after = record.seq;
  }
  return result;
}
std::string bytes(const std::vector<networkLogs::Record> &records) {
  std::string output;
  for (const auto &record : records) output.append(record.text, record.length);
  return output;
}

int main() {
  // Console writes before hook installation still reach the console.
  __wrap_uartWrite(&testUart, 'x');
  assert(forwarded == 1 && networkLogs::stats().nextSeq == 1);
  resetCapture();
  assert(std::strlen(networkLogs::bootId()) == 16);
  assert(romConsole == networkLogs::romPutc);

  // Actual byte and buffer Arduino hooks plus both SDK/newlib aliases. No event
  // is duplicated; stdout/stderr capture does not intercept ordinary files.
  __wrap_uartWrite(&testUart, 'A');
  const uint8_t block[] = {'B', '\0', 'C', '\n'};
  __wrap_uartWriteBuf(&testUart, block, sizeof(block));
  assert(__wrap__write_r(nullptr, 1, "SDK\n", 4) == 4);
  assert(__wrap_esp_vfs_write(nullptr, 2, "ERR\n", 4) == 4);
  assert(__wrap__write_r(nullptr, 3, "file", 4) == 4);
  assert(__wrap_esp_vfs_write(nullptr, 99, "bad", 3) == -1 && errno == EBADF);
  __wrap_uartWrite(nullptr, 'X');
  __wrap_uartWriteBuf(nullptr, block, sizeof(block));
  auto records = readAll();
  assert(records.size() == 4);
  assert(bytes(records) == std::string("AB\0C\nSDK\nERR\n", 13));
  assert(records[0].continued && !records[1].continued);
  assert(forwarded == 9);
  // Reads do not drain or alter sequence numbers.
  assert(bytes(readAll()) == bytes(records));

  resetCapture();
  std::string longLine(300, 'z');
  longLine += '\n';
  __wrap_uartWriteBuf(&testUart,
      reinterpret_cast<const uint8_t *>(longLine.data()), longLine.size());
  records = readAll();
  assert(records.size() == 3 && bytes(records) == longLine);
  assert(records[0].length == 128 && records[0].continued);
  assert(records[1].length == 128 && records[1].continued);
  assert(records[2].length == 45 && !records[2].continued);

  resetCapture();
  for (char c : longLine) romConsole(c);
  records = readAll();
  assert(records.size() == 3 && bytes(records) == longLine);
  for (char c : std::string("partial")) romConsole(c);
  records = readAll();
  assert(records.back().continued && records.back().length == 7);

  resetCapture();
  for (unsigned i = 0; i < 40; ++i) __wrap_uartWrite(&testUart, '0' + i % 10);
  auto stats = networkLogs::stats();
  assert(stats.oldestSeq == 9 && stats.nextSeq == 41 && stats.dropped == 8);
  records = readAll();
  assert(records.size() == 32 && records.front().seq == 9);
  networkLogs::Record record;
  assert(networkLogs::readAfter(20, record) && record.seq == 21);
  assert(!networkLogs::readAfter(UINT32_MAX, record));

  // Multiple callback/task producers share the actual ring implementation.
  // Validate complete records while concurrent reads and overwrite occur.
  resetCapture();
  std::atomic<bool> stopReader{false};
  std::thread reader([&] {
    networkLogs::Record item;
    uint32_t last = 0;
    while (!stopReader.load()) {
      if (networkLogs::readAfter(last, item)) {
        assert(item.seq > last && item.length == 96);
        const unsigned content = item.text[95] == '\n' ? 95 : 96;
        for (unsigned i = 1; i < content; ++i) assert(item.text[i] == item.text[0]);
        last = item.seq;
      }
    }
  });
  std::vector<std::thread> producers;
  for (unsigned p = 0; p < 4; ++p) producers.emplace_back([p] {
    const std::string message(96, 'a' + p);
    for (unsigned i = 0; i < 250; ++i)
      __wrap_uartWriteBuf(&testUart,
          reinterpret_cast<const uint8_t *>(message.data()), message.size());
  });
  producers.emplace_back([] {
    const std::string message = std::string(95, 'r') + '\n';
    for (unsigned i = 0; i < 100; ++i)
      for (char c : message) romConsole(c);
  });
  for (auto &producer : producers) producer.join();
  stopReader.store(true);
  reader.join();
  stats = networkLogs::stats();
  assert(stats.nextSeq == 1101 && stats.dropped == 1068 && stats.oldestSeq == 1069);
  records = readAll();
  assert(records.size() == 32);
}
