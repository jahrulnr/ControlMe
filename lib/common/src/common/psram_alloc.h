#pragma once
// PSRAM-first allocation for large buffers/objects — internal DRAM is
// reserved for lwIP, DMA drivers, and task stacks (internal free can be
// <15 KiB with WiFi+camera active). Internal fallback when PSRAM is
// full/absent. In host tests (no esp_heap_caps) -> plain malloc/free.
//
// Remaining internal-pressure sources: WiFi driver ~35 KiB, esp_camera ~30
// KiB, ~8 task stacks ~50 KiB — all must be internal (stacks can't go to
// PSRAM: CONFIG_SPIRAM_ALLOW_STACK_EXTERNAL_MEMORY off in the prebuilt
// sdkconfig).
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <memory>
#include <new>

#if defined(ESP32) || defined(ARDUINO_ARCH_ESP32)
#include <esp_heap_caps.h>
#define CONTROLME_PSRAM_CAPABLE 1
#endif

namespace controlme {

inline void* psramMalloc(size_t n) {
  if (!n) n = 1;
#ifdef CONTROLME_PSRAM_CAPABLE
  void* p = heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!p) p = heap_caps_malloc(n, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  return p;
#else
  return malloc(n);
#endif
}

inline void* psramCalloc(size_t n, size_t sz) {
  void* p = psramMalloc(n * sz);
  if (p) memset(p, 0, n * sz);
  return p;
}

inline void psramFree(void* p) {
#ifdef CONTROLME_PSRAM_CAPABLE
  heap_caps_free(p);
#else
  free(p);
#endif
}

template <typename T, typename... Args>
T* psramNew(Args&&... args) {
  void* m = psramMalloc(sizeof(T));
  return m ? new (m) T(static_cast<Args&&>(args)...) : nullptr;
}

template <typename T>
void psramDelete(T* p) {
  if (p) {
    p->~T();
    psramFree(p);
  }
}

// RAII: PsramPtr<T> for single objects, PsramArr<T> for POD arrays.
template <typename T>
struct PsramObjDeleter { void operator()(T* p) const { psramDelete(p); } };
template <typename T>
struct PsramArrDeleter { void operator()(T* p) const { psramFree(p); } };
template <typename T>
using PsramPtr = std::unique_ptr<T, PsramObjDeleter<T>>;
template <typename T>
using PsramArr = std::unique_ptr<T[], PsramArrDeleter<T>>;

template <typename T, typename... Args>
PsramPtr<T> psramMake(Args&&... args) {
  return PsramPtr<T>(psramNew<T>(static_cast<Args&&>(args)...));
}
template <typename T>
PsramArr<T> psramArr(size_t n) {
  return PsramArr<T>(static_cast<T*>(psramMalloc(sizeof(T) * n)));
}

}  // namespace controlme
