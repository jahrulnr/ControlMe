#pragma once
// DeviceLog — persistent log in internal LittleFS (/logs/current.log +
// /logs/prev.log, rotated per boot in begin()). All file I/O happens only in
// the FileLog owner task (owner_task.h pattern); the facade blocks callers
// with a bounded deadline — readers never touch the FS directly.
//
// Dual capture (see logCaptureInstall):
//   - Project ESP_LOGx -> Arduino log_x -> log_printfv -> ets_printf -> putc2.
//   - Internal IDF logs (wifi/tcpip/camera) -> esp_log_write -> vprintf ->
//     esp_log_set_vprintf hook.
// Both push bytes into a static DRAM ring (IRAM-safe, no FreeRTOS/PSRAM/
// heap); the owner task drains it to the file periodically.
//
// Timeout rule on read: dst is a caller-owned PSRAM buffer that must stay
// valid until call() returns — the owner writes to dst inside the heap
// request whose ownership transfers on abandon, but dst itself belongs to
// the caller; on Err::Timeout the caller MUST NOT free dst right away (the
// owner may still be writing) — project convention: dst is allocated via
// psramArr and deliberately leaked (ESP_LOGW) on Timeout.
#include <stddef.h>
#include <stdint.h>
#include <FS.h>
#include "common/config.h"
#include "common/error.h"
#include "ports/devlog.h"

// owner_task.h lives in src/ — __has_include resolution + relative
// fallback, same convention as storage/servo/camera.
#if __has_include("owner_task.h")
#include "owner_task.h"
#else
#include "../../../src/owner_task.h"
#endif

namespace controlme {

// Install the capture hooks. MUST be called right after Serial.begin() —
// uart_install_putc() inside Serial.begin resets putc2 to NULL.
void logCaptureInstall();

class DeviceLog : public IDeviceLog {
 public:
  DeviceLog() = default;
  DeviceLog(const DeviceLog&) = delete;
  DeviceLog& operator=(const DeviceLog&) = delete;

  // Rotate + open file + start owner task. Called once from setup()
  // (single-threaded, after LittleFS.begin). Uses Arduino LittleFS directly —
  // this lib is an FS adapter, not a port.
  Status begin();

  // --- IDeviceLog (facade; other tasks, bounded deadline) ---
  Status read(LogFile which, size_t offset, char* dst, size_t cap,
              uint32_t timeoutMs, LogReadResult& out) override;
  Status flush(uint32_t timeoutMs) override;

 private:
  struct LogReq {
    OwnerCtrl ctrl;
    enum class Op : uint8_t { Read, Flush } op;
    LogFile which;
    size_t offset;
    char* dst;
    size_t cap;
    LogReadResult res;
  };

  static void handle(LogReq& r, void* ctx);
  static void idle(void* ctx);      // periodic drain tick (kLogPollMs)
  void drain(bool force);           // owner ctx: ring -> file
  void writeBytes(const char* d, size_t n);  // write + short-write latch
  void maybeTruncate(size_t addBytes);  // atomic keep-tail via current.tmp
  void readFile(const char* path, size_t offset, char* dst, size_t cap,
                LogReadResult& out);
  static const char* pathOf(LogFile which);

  OwnerTask<LogReq> owner_;
  fs::File file_;                   // current.log write handle (owner ctx only)
  size_t cap_ = 0;                  // effective current.log cap
  size_t curSize_ = 0;              // current.log size (owner ctx)
  bool writeEnabled_ = false;
  uint32_t generation_ = 0;         // ++ per keep-tail truncate
  uint32_t lastWriteMs_ = 0;
  char* staging_ = nullptr;         // PSRAM kLogRingCap (drain buffer)
  char* tailBuf_ = nullptr;         // PSRAM ~cap/2 during truncate
  bool shortWriteWarned_ = false;   // short-write WARN latch
};

}  // namespace controlme
