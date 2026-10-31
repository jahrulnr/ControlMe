#include "domain/camera_format.h"
#include <string.h>

namespace controlme {
namespace {
constexpr CameraFormat formats[] = {
  {CameraSize::QQVGA, "QQVGA", 160, 120, "4:3"},
  {CameraSize::Square240, "240X240", 240, 240, "1:1"},
  {CameraSize::QVGA, "QVGA", 320, 240, "4:3"},
  {CameraSize::CIF, "CIF", 400, 296, "50:37"},
  {CameraSize::HVGA, "HVGA", 480, 320, "3:2"},
  {CameraSize::VGA, "VGA", 640, 480, "4:3"},
  {CameraSize::SVGA, "SVGA", 800, 600, "4:3"},
  {CameraSize::XGA, "XGA", 1024, 768, "4:3"},
  {CameraSize::HD, "HD", 1280, 720, "16:9"},
  {CameraSize::SXGA, "SXGA", 1280, 1024, "5:4"},
  {CameraSize::UXGA, "UXGA", 1600, 1200, "4:3"},
};
}
const CameraFormat* cameraFormat(CameraSize size) {
  for (const auto& f : formats) if (f.size == size) return &f;
  return nullptr;
}
const CameraFormat* cameraFormatByName(const char* name) {
  if (!name) return nullptr;
  for (const auto& f : formats) if (!strcmp(f.name, name)) return &f;
  return nullptr;
}
}  // namespace controlme
