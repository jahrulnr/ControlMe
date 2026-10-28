#pragma once
// web_search & web_fetch ports (docs/02): separate vendor search
// (pending review), fetch = bounded HTTP(S) GET without JS. JS-requiring
// targets -> NotSupported.
#include "common/error.h"
#include "ports/writer.h"

namespace controlme {

struct WebSearchItem {
  char title[160] = {};
  char url[256] = {};
  char snippet[256] = {};
};
struct WebSearchResult {
  WebSearchItem items[8];
  uint8_t count = 0;
};

class IWebSearch {
 public:
  virtual ~IWebSearch() = default;
  virtual bool isConfigured() = 0;
  virtual Status search(const char* query, uint8_t limit, WebSearchResult& out) = 0;
};

struct WebFetchMeta {
  int httpStatus = 0;
  char finalUrl[256] = {};
  char mime[64] = {};
};

class IWebFetch {
 public:
  virtual ~IWebFetch() = default;
  // Stream text body to the sink (chunks); meta filled up front.
  virtual Status fetch(const char* url, WebFetchMeta& metaOut, IByteSink& sink) = 0;
};

}  // namespace controlme
