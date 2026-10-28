#include "filelog/device_log.h"

#include <Arduino.h>
#include <LittleFS.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <esp32/rom/ets_sys.h>
#include <freertos/FreeRTOS.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "common/config.h"
#include "common/psram_alloc.h"

namespace controlme {

static const char* kTag = "filelog";
static const char* kPathCurrent = "/logs/current.log";
static const char* kPathPrev = "/logs/prev.log";
static const char* kPathTmp = "/logs/current.tmp";

// ------------------------------------------------------------- capture ring
// Static byte ring in .bss (plain DRAM — DRAM_ATTR is only for fields
// touched by IRAM, see note below; .bss is already DRAM so it's unneeded).
static char s_ring[cfg::kLogRingCap];
static uint32_t s_head = 0;                     // next write position (lock)
static uint32_t s_tail = 0;                     // next read position (lock)
static uint32_t s_count = 0;                    // bytes filled (lock)
// Incremented lock-free when the spinlock is refused — the only lock-free
// access, hence still volatile; drain snapshots it inside the critical
// section.
static volatile uint32_t s_droppedBytes = 0;
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static int (*s_prevVprintf)(const char*, va_list) = nullptr;

// IRAM_ATTR REQUIRED on the whole capture path: putc2 can run while the
// flash cache is off — code that isn't inlined into IRAM = crash, not a
// warning.
static void IRAM_ATTR ringPushLocked(uint8_t c) {
  if (s_count >= cfg::kLogRingCap) {
    s_droppedBytes = s_droppedBytes + 1;
    return;  // drop newest — better for a log to lose data than block an ISR
  }
  s_ring[s_head] = (char)c;
  s_head = (s_head + 1) % cfg::kLogRingCap;
  ++s_count;
}

static void IRAM_ATTR ringPush(const uint8_t* d, size_t n) {
  if (portTRY_ENTER_CRITICAL(&s_mux, portMUX_TRY_LOCK) != pdPASS) {
    s_droppedBytes = s_droppedBytes + 1;  // contended — drop the whole chunk, don't spin in an ISR
    return;
  }
  for (size_t i = 0; i < n; ++i) ringPushLocked(d[i]);
  portEXIT_CRITICAL(&s_mux);
}

static void IRAM_ATTR captureChar(char c) { ringPush((const uint8_t*)&c, 1); }

static int captureVprintf(const char* fmt, va_list ap) {
  va_list aq;
  va_copy(aq, ap);
  // 128 B: used by any task emitting IDF logs, including sys_evt whose
  // stack is 2 KiB — a truncated line beats a large stack.
  char line[128];
  int n = vsnprintf(line, sizeof(line), fmt, aq);
  va_end(aq);
  if (n > 0) {
    size_t len = (size_t)n < sizeof(line) ? (size_t)n : sizeof(line) - 1;
    ringPush((const uint8_t*)line, len);
  }
  // Forward to the original vprintf (UART) — a hook, not a replacement for normal output.
  return s_prevVprintf ? s_prevVprintf(fmt, ap) : n;
}

void logCaptureInstall() {
  ets_install_putc2(&captureChar);
  s_prevVprintf = esp_log_set_vprintf(&captureVprintf);
}

// ---------------------------------------------------------------- helpers

const char* DeviceLog::pathOf(LogFile which) {
  return which == LogFile::Previous ? kPathPrev : kPathCurrent;
}

// Stack low-water: print only on a new per-task minimum (static latch
// per call site — a few lines per boot, never spam).
static void stackHwmNote(const char* name) {
  static uint32_t s_min = UINT32_MAX;
  const uint32_t h = uxTaskGetStackHighWaterMark(nullptr);
  if (h < s_min) {
    s_min = h;
    ESP_LOGI(kTag, "stack %s hwm=%u", name, (unsigned)h);
  }
}

// ------------------------------------------------------------------- begin

Status DeviceLog::begin() {
  // Rotation (setup context, before the task lives): old prev is dropped,
  // current -> prev. On failure -> warn once then continue (an honest log
  // beats dying).
  if (!LittleFS.exists("/logs")) LittleFS.mkdir("/logs");
  if (LittleFS.exists(kPathPrev) && !LittleFS.remove(kPathPrev)) {
    ESP_LOGW(kTag, "remove %s failed — old prev overwritten by rename", kPathPrev);
  }
  if (LittleFS.exists(kPathCurrent) &&
      !LittleFS.rename(kPathCurrent, kPathPrev)) {
    ESP_LOGW(kTag, "rotate %s -> prev failed — appending to old current",
             kPathCurrent);
  }
  size_t prevSize = 0;
  {
    fs::File p = LittleFS.open(kPathPrev, "r");
    if (p) {
      prevSize = p.size();
      p.close();
    }
  }

  const size_t free = LittleFS.totalBytes() - LittleFS.usedBytes();
  cap_ = cfg::kLogFileCap;
  const size_t budget = free > cfg::kLogFsReserve ? free - cfg::kLogFsReserve : 0;
  if (budget < cap_) cap_ = budget;
  writeEnabled_ = cap_ >= cfg::kLogMinCap;
  if (writeEnabled_) {
    file_ = LittleFS.open(kPathCurrent, "a");
    if (file_) {
      curSize_ = file_.size();  // if rotation failed, append to old contents
    } else {
      writeEnabled_ = false;
      ESP_LOGW(kTag, "open %s failed — log writes off", kPathCurrent);
    }
  } else {
    ESP_LOGW(kTag, "log cap %u < min %u (free %u) — log writes off",
             (unsigned)cap_, (unsigned)cfg::kLogMinCap, (unsigned)free);
  }
  ESP_LOGI(kTag, "filelog: cap=%u free=%u prev=%u write=%s", (unsigned)cap_,
           (unsigned)free, (unsigned)prevSize, writeEnabled_ ? "on" : "off");

  staging_ = static_cast<char*>(psramMalloc(cfg::kLogRingCap));
  tailBuf_ = static_cast<char*>(psramMalloc(cfg::kLogFileCap / 2));
  if (!staging_ || !tailBuf_) {
    ESP_LOGW(kTag, "filelog PSRAM buffers failed to allocate");
  }
  return owner_.start(OwnerTaskConfig<LogReq>{
      .name = "filelog",
      .stackBytes = cfg::kStackFileLog,
      .priority = cfg::kPrioFileLog,
      .queueDepth = 4,
      .handle = &DeviceLog::handle,
      .ctx = this,
      .core = cfg::kCoreApp,
      .idleMs = cfg::kLogPollMs,
      .idle = &DeviceLog::idle});
}

// ------------------------------------------------------------------- drain

void DeviceLog::idle(void* ctx) {
  static_cast<DeviceLog*>(ctx)->drain(false);
}

// Snapshot ring + drop counter inside a short critical section (wrap
// split -> max 2 memcpy), write to file outside the critical section.
void DeviceLog::drain(bool force) {
  if (!staging_) return;
  const uint32_t now = millis();
  uint32_t dropped = 0;
  size_t count;
  bool due;
  portENTER_CRITICAL(&s_mux);
  count = s_count;
  // Writes off -> always "due": ring contents discarded so it doesn't sit full.
  due = force || !writeEnabled_ || count >= cfg::kLogWriteBatch ||
        (count > 0 && (now - lastWriteMs_) >= cfg::kLogFlushMs);
  if (due) {
    const uint32_t first =
        (s_tail + count <= cfg::kLogRingCap) ? count
                                             : cfg::kLogRingCap - s_tail;
    memcpy(staging_, s_ring + s_tail, first);
    if (first < count) {
      memcpy(staging_ + first, s_ring, count - first);
    }
    s_tail = s_head;
    s_count = 0;
    dropped = s_droppedBytes;
    s_droppedBytes = 0;
  }
  portEXIT_CRITICAL(&s_mux);
  if (!due || (count == 0 && dropped == 0)) return;

  if (!writeEnabled_) return;  // FS not viable — the log lives on UART only

  // Ring contents FIRST, then the drop marker (drop = newest bytes).
  if (count) {
    maybeTruncate(count);
    if (!writeEnabled_) return;
    writeBytes(staging_, count);
  }
  if (dropped) {
    char m[48];
    int mn = snprintf(m, sizeof(m), "[filelog] dropped %lu bytes\n",
                      (unsigned long)dropped);
    if (mn > 0) {
      maybeTruncate((size_t)mn);
      if (writeEnabled_) writeBytes(m, (size_t)mn);
    }
  }
  file_.flush();
}

// Write + record short-writes (lost bytes -> drop counter; WARN once,
// latched until the next full write — the warn must not be re-captured).
void DeviceLog::writeBytes(const char* d, size_t n) {
  const size_t w = file_.write((const uint8_t*)d, n);
  curSize_ += w;
  lastWriteMs_ = millis();
  if (w < n) {
    portENTER_CRITICAL(&s_mux);
    s_droppedBytes = s_droppedBytes + (n - w);
    portEXIT_CRITICAL(&s_mux);
    if (!shortWriteWarned_) {
      shortWriteWarned_ = true;
      ESP_LOGW(kTag, "short write %u/%u — FS full?", (unsigned)w,
               (unsigned)n);
    }
  } else {
    shortWriteWarned_ = false;
  }
}

// Atomic keep-tail: size+add past cap -> tail ~cap/2 -> current.tmp ->
// remove(current) + rename(tmp,current) -> reopen "a". Power loss mid-way
// leaves the old OR the new file — never a half.
void DeviceLog::maybeTruncate(size_t addBytes) {
  if (!writeEnabled_ || curSize_ + addBytes <= cap_) return;
  const size_t keep = cap_ / 2;
  size_t n = 0;
  if (tailBuf_) {
    fs::File r = LittleFS.open(kPathCurrent, "r");
    if (r) {
      const size_t sz = r.size();
      const size_t start = sz > keep ? sz - keep : 0;
      r.seek(start);
      n = r.read((uint8_t*)tailBuf_, keep);
      r.close();
      // Advance to the start of the next whole line.
      size_t cut = 0;
      while (cut < n && tailBuf_[cut] != '\n') ++cut;
      if (cut < n) ++cut;
      memmove(tailBuf_, tailBuf_ + cut, n - cut);
      n -= cut;
    }
  }
  {
    fs::File t = LittleFS.open(kPathTmp, "w");
    if (t) {
      if (n) t.write((const uint8_t*)tailBuf_, n);
      t.close();
    }
  }
  file_.close();
  if (!LittleFS.remove(kPathCurrent) || !LittleFS.rename(kPathTmp, kPathCurrent)) {
    ESP_LOGW(kTag, "truncate: rename tmp->current failed — old file lost");
  }
  file_ = LittleFS.open(kPathCurrent, "a");
  if (!file_) {
    writeEnabled_ = false;
    ESP_LOGW(kTag, "truncate: reopen %s failed — writes off", kPathCurrent);
    return;
  }
  curSize_ = file_.size();
  ++generation_;
  ESP_LOGI(kTag, "truncate keep-tail: size->%u gen=%lu", (unsigned)curSize_,
           (unsigned long)generation_);
  stackHwmNote("filelog");
}

// ------------------------------------------------------------------- owner

void DeviceLog::readFile(const char* path, size_t offset, char* dst,
                         size_t cap, LogReadResult& out) {
  out = LogReadResult{};
  out.generation = generation_;
  fs::File f = LittleFS.open(path, "r");
  if (!f) {
    out.eof = true;  // missing file -> empty, not an error
    return;
  }
  out.size = f.size();
  if (offset < out.size && dst && cap) {
    f.seek(offset);
    out.outLen = f.read((uint8_t*)dst, cap);
  }
  f.close();
  out.eof = offset + out.outLen >= out.size;
}

void DeviceLog::handle(LogReq& r, void* ctx) {
  DeviceLog& self = *static_cast<DeviceLog*>(ctx);
  switch (r.op) {
    case LogReq::Op::Read:
      if (r.which == LogFile::Current) self.drain(true);  // freshest data first
      self.readFile(pathOf(r.which), r.offset, r.dst, r.cap, r.res);
      r.ctrl.status = Status::Ok();
      break;
    case LogReq::Op::Flush:
      self.drain(true);
      r.ctrl.status = Status::Ok();
      break;
  }
}

// ------------------------------------------------------------------ facade

Status DeviceLog::read(LogFile which, size_t offset, char* dst, size_t cap,
                       uint32_t timeoutMs, LogReadResult& out) {
  LogReq in{}, res{};
  in.op = LogReq::Op::Read;
  in.which = which;
  in.offset = offset;
  in.dst = dst;
  in.cap = cap;
  Status st = owner_.call(in, res, timeoutMs);
  if (st.ok()) out = res.res;
  return st;
}

Status DeviceLog::flush(uint32_t timeoutMs) {
  LogReq in{}, res{};
  in.op = LogReq::Op::Flush;
  return owner_.call(in, res, timeoutMs);
}

}  // namespace controlme
