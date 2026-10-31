#pragma once
#include <stddef.h>
#include "common/error.h"

namespace controlme {

class IContractStore {
 public:
  virtual ~IContractStore() = default;
  // Empty reference selects the core contract. Returns the entire document.
  virtual Status read(const char* reference, char* buffer, size_t capacity,
                      size_t& length) = 0;
};

}  // namespace controlme
