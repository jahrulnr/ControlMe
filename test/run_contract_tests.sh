#!/usr/bin/env bash
set -eu
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT="$(mktemp -d)"
trap 'rm -rf "$OUT"' EXIT
"${CXX:-g++}" -std=c++17 -Wall -Wextra -fsanitize=address,undefined \
  -fno-omit-frame-pointer -g -I"$ROOT/test/contract_fakes" -I"$ROOT/src" \
  -I"$ROOT/lib/ports/src" -I"$ROOT/lib/common/src" -I"$ROOT/lib/actions/src" \
  "$ROOT/src/contract_store.cpp" "$ROOT/lib/actions/src/actions/contract_result.cpp" \
  "$ROOT/lib/common/src/common/json_escape.cpp" "$ROOT/test/contract_store/test_contract_store.cpp" \
  -o "$OUT/test_contract_store"
"$OUT/test_contract_store"
echo "PASS contract_store (LittleFS fake)"
