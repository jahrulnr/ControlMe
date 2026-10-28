#pragma once
// Project logging fix-up (ESP32 only — host tests: no-op).
//
// The prebuilt Arduino-ESP32 framework sdkconfig uses
// CONFIG_LOG_MAXIMUM_LEVEL=1 (ERROR). As a result, the IDF esp_log.h
// ESP_LOGI/ESP_LOGW/ESP_LOGD/ESP_LOGV are compiled out ENTIRELY — "camera
// ready", sccb probes, etc. never come out even with a runtime
// esp_log_level_set(INFO) call.
//
// esp32-hal-log.h itself includes esp_log.h then #undef+redefines ESP_LOGx
// -> Arduino log_x (honoring CORE_DEBUG_LEVEL=3 in platformio.ini). That
// redefinition sticks because esp_log.h has an include guard — include this
// header via common/error.h (used by every TU needing Status) for uniform
// [ms][LEVEL][file:line] logging.
#if __has_include(<esp32-hal-log.h>)
#include <esp32-hal-log.h>
#endif
