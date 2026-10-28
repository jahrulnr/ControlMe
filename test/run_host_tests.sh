#!/usr/bin/env bash
# Host tests for pure domain code (no ESP-IDF/Arduino).
# Convention: each suite lives in test/host_<name>/*.cpp with one main() per file.
# Compiled with all pure lib sources: lib/common, lib/domain, lib/llmproto,
# lib/mcpproto — with ASan/UBSan.
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
CXX="${CXX:-g++}"
STD="-std=gnu++17 -Wall -Wextra -Wno-unused-parameter -fsanitize=address,undefined -fno-omit-frame-pointer -g"
INC="-I$ROOT/lib/common/src -I$ROOT/lib/domain/src -I$ROOT/lib/llmproto/src -I$ROOT/lib/mcpproto/src -I$ROOT/lib/ports/src"
PURE_SRC="$(find "$ROOT/lib/common/src" "$ROOT/lib/domain/src" "$ROOT/lib/llmproto/src" "$ROOT/lib/mcpproto/src" -name '*.cpp' 2>/dev/null)"
OUT="$(mktemp -d)"
fail=0

for d in "$ROOT"/test/host_*/; do
  [ -d "$d" ] || continue
  name="$(basename "$d")"
  for src in "$d"*.cpp; do
    [ -f "$src" ] || continue
    bin="$OUT/$(basename "$src" .cpp)"
    if ! $CXX $STD $INC "$src" $PURE_SRC -o "$bin" 2> "$bin.build.log"; then
      echo "BUILD FAIL  $name/$(basename "$src")"
      cat "$bin.build.log"
      fail=1
      continue
    fi
    if "$bin" > "$bin.run.log" 2>&1; then
      echo "PASS  $name/$(basename "$src")"
    else
      echo "FAIL  $name/$(basename "$src")"
      cat "$bin.run.log"
      fail=1
    fi
  done
done

exit $fail
