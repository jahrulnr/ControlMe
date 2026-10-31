#pragma once
#include "ports/contract.h"
#include "ports/writer.h"

namespace controlme {

// Buffers belong to the caller; fail rather than returning truncated guidance.
Status writeContractResult(IContractStore& store, const char* reference,
                           IByteSink& out, char* text, size_t textCap,
                           char* escaped, size_t escapedCap);

}  // namespace controlme
