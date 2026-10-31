#include <assert.h>
#include <string>
#include "LittleFS.h"
#include "contract_store.h"
#include "actions/contract_result.h"

FakeLittleFS LittleFS;
extern const uint8_t core[] asm("_binary_contracts_core_md_start") = "core text";
extern const uint8_t camera[] asm("_binary_contracts_camera_md_start") = "camera text";
extern const uint8_t camera_format[] asm("_binary_contracts_camera_format_md_start") = "camera format text";
extern const uint8_t motion[] asm("_binary_contracts_motion_md_start") = "motion text";
extern const uint8_t storage[] asm("_binary_contracts_storage_md_start") = "storage text";
extern const uint8_t device[] asm("_binary_contracts_device_md_start") = "device text";

class StringSink : public controlme::IByteSink {
 public:
  std::string value;
  controlme::Status write(const uint8_t* data, size_t size) override {
    value.append(reinterpret_cast<const char*>(data), size);
    return controlme::Status::Ok();
  }
};

int main() {
  using controlme::Err;
  auto* store = controlme_install_contracts();
  assert(store && LittleFS.writes == 6);
  assert(LittleFS.files["/contracts/core.md"] == "core text");
  char buf[64];
  size_t len = 99;
  assert(store->read("", buf, sizeof(buf), len).ok());
  assert(std::string(buf, len) == "core text");
  assert(store->read("camera", buf, sizeof(buf), len).ok());
  assert(std::string(buf, len) == "camera text");
  assert(store->read("missing", buf, sizeof(buf), len).code == Err::NotFound);
  assert(store->read("../logs/current", buf, sizeof(buf), len).code == Err::InvalidArgument);
  assert(store->read("camera", buf, 2, len).code == Err::Internal);
  LittleFS.files["/contracts/custom.md"] = "line\n\"quoted\"\\path";
  char escaped[128];
  StringSink sink;
  assert(controlme::writeContractResult(*store, "custom", sink, buf, sizeof(buf),
                                        escaped, sizeof(escaped)).ok());
  assert(sink.value == "{\"content\":\"line\\n\\\"quoted\\\"\\\\path\"}");
  StringSink missing;
  assert(controlme::writeContractResult(*store, "missing", missing, buf, sizeof(buf),
                                        escaped, sizeof(escaped)).code == Err::NotFound);
  assert(missing.value.empty());
  StringSink tooSmall;
  assert(controlme::writeContractResult(*store, "custom", tooSmall, buf, sizeof(buf),
                                        escaped, 3).code == Err::Internal);
  assert(tooSmall.value.empty());
  assert(controlme_install_contracts() && LittleFS.writes == 6);
  LittleFS.files["/contracts/core.md"] = "older version";
  LittleFS.failRename = true;
  assert(!controlme_install_contracts());
  assert(LittleFS.files["/contracts/core.md"] == "older version");
  assert(!LittleFS.files.count("/contracts/.install.tmp"));
  LittleFS.failRename = false;
  assert(controlme_install_contracts());
  assert(LittleFS.files["/contracts/core.md"] == "core text");
}
