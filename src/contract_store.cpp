#include <LittleFS.h>
#include <esp_log.h>
#include <stdio.h>
#include <string.h>
#include "contract_store.h"

extern const uint8_t core_start[] asm("_binary_contracts_core_md_start");
extern const uint8_t camera_start[] asm("_binary_contracts_camera_md_start");
extern const uint8_t camera_format_start[] asm("_binary_contracts_camera_format_md_start");
extern const uint8_t motion_start[] asm("_binary_contracts_motion_md_start");
extern const uint8_t storage_start[] asm("_binary_contracts_storage_md_start");
extern const uint8_t device_start[] asm("_binary_contracts_device_md_start");

namespace {
using namespace controlme;

struct BundledContract { const char* name; const uint8_t* text; };
const BundledContract kBundled[] = {
    {"core", core_start}, {"camera", camera_start}, {"camera-format", camera_format_start},
    {"motion", motion_start},
    {"storage", storage_start}, {"device", device_start},
};

// Reference names map to files within /contracts, not arbitrary FS paths.
bool contractPath(const char* reference, char* path, size_t cap) {
  const char* name = reference && *reference ? reference : "core";
  size_t n = strlen(name);
  if (n > 48) return false;
  for (size_t i = 0; i < n; ++i) {
    const char c = name[i];
    if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
          c == '-' || c == '_')) return false;
  }
  int written = snprintf(path, cap, "/contracts/%s.md", name);
  return written >= 0 && static_cast<size_t>(written) < cap;
}

bool install(const BundledContract& doc) {
  char path[80];
  if (!contractPath(doc.name, path, sizeof(path))) return false;
  const size_t size = strlen(reinterpret_cast<const char*>(doc.text));
  fs::File old = LittleFS.open(path, "r");
  bool same = old && old.size() == size;
  if (same) {
    for (size_t i = 0; i < size; ++i) {
      if (old.read() != doc.text[i]) { same = false; break; }
    }
  }
  old.close();
  if (same) return true;

  const char* tmp = "/contracts/.install.tmp";
  fs::File file = LittleFS.open(tmp, "w");
  if (!file) return false;
  const size_t written = file.write(doc.text, size);
  file.flush();
  const bool ok = written == size && !file.getWriteError();
  file.close();
  if (!ok || !LittleFS.rename(tmp, path)) {
    LittleFS.remove(tmp);
    return false;
  }
  return true;
}

class LittleFsContracts : public IContractStore {
 public:
  Status read(const char* reference, char* buffer, size_t capacity,
              size_t& length) override {
    length = 0;
    char path[80];
    if (!contractPath(reference, path, sizeof(path)))
      return Status::Fail(Err::InvalidArgument, "invalid contract reference");
    fs::File file = LittleFS.open(path, "r");
    if (!file || file.isDirectory()) return Status::Fail(Err::NotFound, "contract not found");
    const size_t size = file.size();
    if (size > capacity) return Status::Fail(Err::Internal, "contract too large");
    length = file.readBytes(buffer, size);
    if (length != size) return Status::Fail(Err::Internal, "contract read failed");
    return Status::Ok();
  }
};
}  // namespace

controlme::IContractStore* controlme_install_contracts() {
  if (!LittleFS.exists("/contracts") && !LittleFS.mkdir("/contracts")) {
    ESP_LOGE("contract", "cannot create contract directory");
    return nullptr;
  }
  for (const auto& doc : kBundled) {
    if (!install(doc)) {
      ESP_LOGE("contract", "cannot install %s", doc.name);
      return nullptr;
    }
  }
  static LittleFsContracts store;
  return &store;
}
