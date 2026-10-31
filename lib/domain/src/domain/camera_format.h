#pragma once
#include "ports/camera.h"

namespace controlme {
struct CameraFormat {
  CameraSize size;
  const char* name;
  uint16_t width;
  uint16_t height;
  const char* ratio;
};
const CameraFormat* cameraFormat(CameraSize size);
const CameraFormat* cameraFormatByName(const char* name);
}  // namespace controlme
