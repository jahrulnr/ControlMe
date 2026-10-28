#include "common/error.h"

#include <string.h>

namespace controlme {

bool errFromName(const char* name, Err& out) {
  if (!name || !*name) return false;
  for (uint8_t i = 0; i <= (uint8_t)Err::Internal; ++i) {
    if (strcmp(name, errName((Err)i)) == 0) {
      out = (Err)i;
      return true;
    }
  }
  return false;
}

const char* errName(Err e) {
  switch (e) {
    case Err::None: return "NONE";
    case Err::InvalidArgument: return "INVALID_ARGUMENT";
    case Err::NotSupported: return "NOT_SUPPORTED";
    case Err::NotFound: return "NOT_FOUND";
    case Err::FileBusy: return "FILE_BUSY";
    case Err::RevisionConflict: return "REVISION_CONFLICT";
    case Err::ResourceBusy: return "RESOURCE_BUSY";
    case Err::UnknownExecution: return "UNKNOWN_EXECUTION";
    case Err::ImageTooLarge: return "IMAGE_TOO_LARGE";
    case Err::QueueFull: return "QUEUE_FULL";
    case Err::DeviceRebooting: return "DEVICE_REBOOTING";
    case Err::SdUnavailable: return "SD_UNAVAILABLE";
    case Err::ProviderUnavailable: return "PROVIDER_UNAVAILABLE";
    case Err::Timeout: return "TIMEOUT";
    case Err::ProtocolIncomplete: return "PROTOCOL_INCOMPLETE";
    case Err::Internal: return "INTERNAL";
  }
  return "INTERNAL";
}

int errHttpStatus(Err e) {
  switch (e) {
    case Err::None: return 200;
    case Err::InvalidArgument: return 422;
    case Err::NotSupported: return 422;
    case Err::NotFound: return 404;
    case Err::FileBusy: return 409;
    case Err::RevisionConflict: return 409;
    case Err::ResourceBusy: return 409;
    case Err::UnknownExecution: return 409;
    case Err::ImageTooLarge: return 413;
    case Err::QueueFull: return 429;
    case Err::DeviceRebooting: return 503;
    case Err::SdUnavailable: return 503;
    case Err::ProviderUnavailable: return 503;
    case Err::Timeout: return 504;
    case Err::ProtocolIncomplete: return 502;
    case Err::Internal: return 500;
  }
  return 500;
}

const char* errTitle(Err e) {
  switch (e) {
    case Err::None: return "OK";
    case Err::InvalidArgument: return "Invalid Argument";
    case Err::NotSupported: return "Not Supported";
    case Err::NotFound: return "Not Found";
    case Err::FileBusy: return "File Busy";
    case Err::RevisionConflict: return "Revision Conflict";
    case Err::ResourceBusy: return "Resource Busy";
    case Err::UnknownExecution: return "Unknown Execution";
    case Err::ImageTooLarge: return "Image Too Large";
    case Err::QueueFull: return "Queue Full";
    case Err::DeviceRebooting: return "Device Rebooting";
    case Err::SdUnavailable: return "SD Unavailable";
    case Err::ProviderUnavailable: return "Provider Unavailable";
    case Err::Timeout: return "Timeout";
    case Err::ProtocolIncomplete: return "Protocol Incomplete";
    case Err::Internal: return "Internal Error";
  }
  return "Internal Error";
}

}  // namespace controlme
