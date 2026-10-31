#include "domain/camera_format.h"
#include <assert.h>
#include <string.h>
using namespace controlme;
int main() {
  const auto* vga = cameraFormatByName("VGA");
  assert(vga && vga->width == 640 && vga->height == 480 && !strcmp(vga->ratio, "4:3"));
  const auto* hd = cameraFormatByName("HD");
  assert(hd && hd->width == 1280 && hd->height == 720 && !strcmp(hd->ratio, "16:9"));
  const auto* square = cameraFormatByName("240X240");
  assert(square && square->width == 240 && square->height == 240 && !strcmp(square->ratio, "1:1"));
  assert(!cameraFormatByName("320X320"));
  assert(cameraFormat(CameraSize::QVGA) == cameraFormatByName("QVGA"));
  assert(!cameraFormatByName("nope") && !cameraFormatByName(nullptr));
  assert(!cameraFormat(static_cast<CameraSize>(255)));
}
