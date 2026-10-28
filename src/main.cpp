// Composition root — the ONLY place of concrete wiring (docs/04).
// setup() builds adapters + transports + routes; all work runs in
// their own tasks — loop() is intentionally empty.
#include <Arduino.h>
#include <LittleFS.h>
#include <WiFi.h>
#include <esp_event.h>
#include <esp_heap_caps.h>
#include <esp_netif.h>
#include <esp_log.h>
#include <esp_random.h>
#include <esp_timer.h>
#include <nvs.h>
#include <nvs_flash.h>
#include <cJSON.h>
#include <sys/time.h>
#include "app.h"
#include "common/psram_alloc.h"
#include "common/event.h"
#include "filelog/device_log.h"
#include "supervisor/reboot_supervisor.h"
// Optional local credentials (include/secret.h, gitignored): WIFI_SSID /
// WIFI_PASSWORD. No file -> empty macros and NVS stays the primary source.
#if __has_include("secret.h")
#include "secret.h"
#endif

static const char* kTag = "app";
static controlme::App g_app;

// ---------------------------------------------------------------- IClock
// mono from esp_timer; wall from gettimeofday — valid only when SNTP is
// synced. SNTP not configured yet -> wallSynced() always false (honest).
class EspClock : public controlme::IClock {
 public:
  uint64_t monoMs() override {
    return (uint64_t)(esp_timer_get_time() / 1000ULL);
  }
  uint64_t wallMs() override {
    struct timeval tv;
    gettimeofday(&tv, nullptr);
    return (uint64_t)tv.tv_sec * 1000ULL + (uint64_t)tv.tv_usec / 1000ULL;
  }
  bool wallSynced() override { return false; }
};
static EspClock g_clock;

// ------------------------------------------------------------- IEventSink
// Assigns sequence + id "<bootId>:<seq>", ESP_LOGI one compact line per
// event. One mutex guarantees numbering order. No ring/fan-out — the main
// event consumer is the persistent log (DeviceLog) via ESP_LOGx capture.
class LogSink : public controlme::IEventSink {
 public:
  explicit LogSink(controlme::App& app) : app_(app) {
    mux_ = xSemaphoreCreateMutex();
  }

  controlme::Status publish(const controlme::Event& ev) override {
    if (!mux_) {
      return controlme::Status::Fail(controlme::Err::Internal, "sink mux");
    }
    controlme::Event e = ev;
    xSemaphoreTake(mux_, portMAX_DELAY);
    const uint32_t seq = ++seq_;
    e.sequence = seq;
    controlme::copyStr(e.bootId, sizeof(e.bootId), app_.bootId);
    snprintf(e.id, sizeof(e.id), "%s:%lu", app_.bootId, (unsigned long)seq);
    app_.lastEventSequence = seq;
    xSemaphoreGive(mux_);
    ESP_LOGI(kTag, "event %s %s src=%s payload=%s", e.id,
             controlme::eventKindName(e.kind), e.source, e.payload);
    return controlme::Status::Ok();
  }

 private:
  controlme::App& app_;
  SemaphoreHandle_t mux_ = nullptr;
  uint32_t seq_ = 0;
};

// ---------------------------------------------------------------- helpers

static const char* resetReasonName(esp_reset_reason_t r) {
  switch (r) {
    case ESP_RST_POWERON: return "POWERON";
    case ESP_RST_SW: return "SW";
    case ESP_RST_PANIC: return "PANIC";
    case ESP_RST_INT_WDT: return "INT_WDT";
    case ESP_RST_TASK_WDT: return "TASK_WDT";
    case ESP_RST_WDT: return "WDT";
    case ESP_RST_BROWNOUT: return "BROWNOUT";
    case ESP_RST_DEEPSLEEP: return "DEEPSLEEP";
    case ESP_RST_EXT: return "EXT";
    case ESP_RST_SDIO: return "SDIO";
    default: return "UNKNOWN";
  }
}

static void initNvs() {
  esp_err_t err = nvs_flash_init();
  if (err == ESP_ERR_NVS_NO_FREE_PAGES ||
      err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    nvs_flash_erase();
    err = nvs_flash_init();
  }
  if (err != ESP_OK) ESP_LOGW(kTag, "nvs_flash_init failed: %d", (int)err);
}

// NVS "partner": wifi_ssid / wifi_pass; build_flags macro fallback.
// The namespace name STAYS "partner" — credentials + device reboot marker
// are already written there.
static bool loadWifiCreds(char* ssid, size_t ssidCap, char* pass,
                        size_t passCap) {
  nvs_handle_t h;
  if (nvs_open("partner", NVS_READONLY, &h) == ESP_OK) {
    size_t l = ssidCap;
    const bool got =
        nvs_get_str(h, "wifi_ssid", ssid, &l) == ESP_OK && ssid[0] != '\0';
    if (got) {
      l = passCap;
      nvs_get_str(h, "wifi_pass", pass, &l);
    }
    nvs_close(h);
    if (got) return true;
  }
#if defined(WIFI_SSID) && defined(WIFI_PASSWORD)
  controlme::copyStr(ssid, ssidCap, WIFI_SSID);
  controlme::copyStr(pass, passCap, WIFI_PASSWORD);
  return ssid[0] != '\0';
#else
  return false;
#endif
}

static void connectWifi() {
  char ssid[64] = {}, pass[64] = {};
  // lwIP/tcpip task + netif MUST be up before httpd/socket — socket() on
  // an un-initialized stack -> assert sys_mutex_lock (measured boot loop).
  // Without credentials: esp_netif_init only — WiFi.mode() allocates ~30 KiB
  // of internal radio driver, wasted when offline (internal DRAM critical).
  if (!loadWifiCreds(ssid, sizeof(ssid), pass, sizeof(pass))) {
    esp_netif_init();
    esp_event_loop_create_default();
    ESP_LOGW(kTag, "wifi: no credentials (NVS wifi_ssid / macro) — LAN off");
    return;
  }
  // Honest status when connect is slow / DHCP IP changes: event (not poll).
  WiFi.onEvent(
      [](arduino_event_t*) {
        g_app.wifiConnected = true;
        snprintf(g_app.httpBaseUrl, sizeof(g_app.httpBaseUrl), "http://%s",
                 WiFi.localIP().toString().c_str());
        ESP_LOGI(kTag, "wifi: got ip %s", g_app.httpBaseUrl);
      },
      ARDUINO_EVENT_WIFI_STA_GOT_IP);
  WiFi.onEvent(
      [](arduino_event_t*) {
        g_app.wifiConnected = false;  // last httpBaseUrl kept
        ESP_LOGW(kTag, "wifi: disconnected");
      },
      ARDUINO_EVENT_WIFI_STA_DISCONNECTED);
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);  // modem-sleep adds per-ACK DTIM latency → slow HTTP
  WiFi.begin(ssid, pass);
  const uint32_t deadline = millis() + controlme::cfg::kConnectTimeoutMs;
  while (WiFi.status() != WL_CONNECTED && millis() < deadline) {
    delay(250);
  }
  if (WiFi.status() == WL_CONNECTED) {
    g_app.wifiConnected = true;
    snprintf(g_app.httpBaseUrl, sizeof(g_app.httpBaseUrl), "http://%s",
             WiFi.localIP().toString().c_str());
    ESP_LOGI(kTag, "wifi: connected %s", g_app.httpBaseUrl);
  } else {
    ESP_LOGW(kTag, "wifi: connect timeout — continuing without network");
  }
}

// ---------------------------------------------------------------- setup

// Modules (src/mod_*.cpp) — registration order matters:
// storage -> servo (the coordinator captures app.storage at register) ->
// other modules -> core.
void controlme_register_storage(controlme::App&);
void controlme_register_servo(controlme::App&);
void controlme_register_camera(controlme::App&);
void controlme_register_log(controlme::App&);
void controlme_register_mcp(controlme::App&);
void controlme_register_supervisor(controlme::App&);

void setup() {
  Serial.begin(115200);
  // Capture log FIRST thing after Serial.begin — uart_install_putc()
  // inside it resets putc2; registering earlier means the hook is lost.
  controlme::logCaptureInstall();
  // Early boot milestone — also the first line into the filelog ring.
  ESP_LOGI(kTag, "boot: init reset=%s", resetReasonName(esp_reset_reason()));
  initNvs();
  ESP_LOGI(kTag, "boot: nvs ok");

  // All cJSON allocations (tool args etc.) -> PSRAM-first: JSON objects
  // can be several KiB and internal DRAM is reserved for lwIP/drivers/stacks.
  static const cJSON_Hooks kJsonHooks = {controlme::psramMalloc,
                                         controlme::psramFree};
  cJSON_InitHooks(const_cast<cJSON_Hooks*>(&kJsonHooks));
  ESP_LOGI(kTag, "boot: heap hooks ok");

  // New bootId per boot — used in event id "<bootId>:<seq>" (kIdCap 24,
  // seq can be 10 digits) so bootId is capped at 9 chars: "b-" + 8 hex.
  snprintf(g_app.bootId, sizeof(g_app.bootId), "b-%08lx",
           (unsigned long)((esp_timer_get_time() / 1000ULL) ^ esp_random()));
  g_app.clock = &g_clock;

  g_app.littleFsMounted = LittleFS.begin(true);  // formatOnFail
  ESP_LOGI(kTag, "boot: littlefs %s",
           g_app.littleFsMounted ? "ok" : "failed");
  // DeviceLog as early as possible — rotate current->prev then start
  // draining the capture ring (the previous boot's log already waits in the
  // ring).
  if (g_app.littleFsMounted) {
    static controlme::DeviceLog g_log;
    if (g_log.begin().ok()) {
      g_app.logs = &g_log;
    } else {
      ESP_LOGW(kTag, "filelog begin failed — log UART-only");
    }
  }

  connectWifi();

  static LogSink sink(g_app);
  g_app.events = &sink;

  controlme::Status st = g_app.http.begin(80);
  if (!st.ok()) {
    ESP_LOGE(kTag, "http :80 failed: %s", st.detail ? st.detail : "?");
  }
  controlme_register_storage(g_app);
  controlme_register_servo(g_app);
  controlme_register_camera(g_app);
  controlme_register_log(g_app);
  controlme_register_mcp(g_app);
  controlme_register_supervisor(g_app);

  // Mount SD via the coordinator BEFORE servo13 PWM may run (docs/05 #8):
  // suspend -> GPIO13 HIGH -> mount -> restore. A failed mount still
  // restores the pin to PWM; servo_set is RESOURCE_BUSY only during the
  // handoff.
  if (g_app.coord) {
    controlme::Status ms = g_app.coord->mountSd();
    if (!ms.ok()) {
      ESP_LOGW(kTag, "mount SD via coordinator failed: %s",
               controlme::errName(ms.code));
    }
  }

  controlme_register_core(g_app);   // core API routes

  // Previous-boot reboot marker (docs/03 #7): read once then cleared;
  // exposed as an event so client reconciliation can flag old requestIds
  // UNKNOWN.
  {
    char prevReason[64] = {}, prevReq[controlme::cfg::kRequestIdCap] = {};
    if (controlme::RebootSupervisor::readRebootMarker(
            prevReason, sizeof(prevReason), prevReq, sizeof(prevReq))) {
      controlme::Event pe{};
      pe.kind = controlme::EventKind::Custom;
      controlme::copyStr(pe.source, sizeof(pe.source), "firmware");
      pe.occurredMonoMs = g_clock.monoMs();
      char esc[64];
      char escReq[54];
      transport::jsonEscape(prevReason, esc, sizeof(esc));
      transport::jsonEscape(prevReq, escReq, sizeof(escReq));
      snprintf(pe.payload, sizeof(pe.payload),
               "{\"lastReboot\":{\"reason\":\"%s\",\"requestId\":\"%s\"}}",
               esc, escReq);
      sink.publish(pe);
      ESP_LOGI(kTag, "previous-boot reboot marker: reason=%s req=%s", prevReason,
               prevReq);
    }
  }

  // DEVICE_READY — produced by firmware after init (docs/03).
  controlme::Event ev{};
  ev.kind = controlme::EventKind::DeviceReady;
  controlme::copyStr(ev.source, sizeof(ev.source), "firmware");
  ev.occurredMonoMs = g_clock.monoMs();
  if (g_app.wifiConnected) {
    snprintf(ev.payload, sizeof(ev.payload), "{\"ip\":\"%s\"}",
             WiFi.localIP().toString().c_str());
  } else {
    controlme::copyStr(ev.payload, sizeof(ev.payload), "{}");
  }
  sink.publish(ev);

  ESP_LOGI(kTag,
           "boot %s done — heap free %u, internal free %u (block %u)",
           g_app.bootId, (unsigned)esp_get_free_heap_size(),
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL |
                                             MALLOC_CAP_8BIT),
           (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL |
                                                      MALLOC_CAP_8BIT));

  // Verify the scheduling contract (config.h): print each task's
  // name/prio/core. Once at boot — on-device proof the actual pin+prio match
  // the table.
  {
    const UBaseType_t n = uxTaskGetNumberOfTasks();
    TaskStatus_t* ts = (TaskStatus_t*)controlme::psramMalloc(
        n * sizeof(TaskStatus_t));
    if (ts) {
      uint32_t rt = 0;
      const UBaseType_t got = uxTaskGetSystemState(ts, n, &rt);
      for (UBaseType_t i = 0; i < got; ++i) {
        ESP_LOGI("sched", "%-14s prio=%-2u core=%-2d stackHWM=%u",
                 ts[i].pcTaskName, (unsigned)ts[i].uxCurrentPriority,
                 (int)ts[i].xCoreID, (unsigned)ts[i].usStackHighWaterMark);
      }
      controlme::psramFree(ts);
    }
  }
}

void loop() {
  vTaskDelete(nullptr);  // loopTask unused — free its ~8KB stack
}
