#include "actions/contract_result.h"
#include "common/json_escape.h"
#include <string.h>

namespace controlme {

Status writeContractResult(IContractStore& store, const char* reference,
                           IByteSink& out, char* text, size_t textCap,
                           char* escaped, size_t escapedCap) {
  size_t length = 0;
  Status st = store.read(reference, text, textCap, length);
  if (!st.ok()) return st;
  size_t escapedLen = 0;
  size_t consumed = jsonEscapeBounded(text, length, escaped, escapedCap,
                                      &escapedLen, true);
  if (consumed != length) return Status::Fail(Err::Internal, "contract result too large");
  const char* head = "{\"content\":\"";
  st = out.write(reinterpret_cast<const uint8_t*>(head), strlen(head));
  if (st.ok()) st = out.write(reinterpret_cast<const uint8_t*>(escaped), escapedLen);
  if (st.ok()) st = out.write(reinterpret_cast<const uint8_t*>("\"}"), 2);
  return st;
}

}  // namespace controlme
