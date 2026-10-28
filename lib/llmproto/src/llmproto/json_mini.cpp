#include "llmproto/json_mini.h"

#include <stdlib.h>
#include <string.h>

namespace controlme::llmproto {

JsonErr JsonDoc::parse(const char* data, size_t len) {
  nodeUsed_ = 0;
  strUsed_ = 0;
  root_ = nullptr;
  err_ = JsonErr::Ok;
  if (len > kJsonInputMax) return JsonErr::InputTooLarge;
  p_ = data;
  end_ = data + len;
  skipWs();
  root_ = parseValue(0);
  if (!root_) return err_;
  skipWs();
  if (p_ != end_) {  // trailing garbage
    err_ = JsonErr::Malformed;
    root_ = nullptr;
    return err_;
  }
  return JsonErr::Ok;
}

JsonValue* JsonDoc::allocNode() {
  if (nodeUsed_ >= kJsonNodeMax) {
    err_ = JsonErr::NodePoolFull;
    return nullptr;
  }
  JsonValue* v = &nodes_[nodeUsed_++];
  *v = JsonValue{};
  return v;
}

void JsonDoc::skipWs() {
  while (p_ < end_ && (*p_ == ' ' || *p_ == '\t' || *p_ == '\n' || *p_ == '\r'))
    ++p_;
}

bool JsonDoc::pushChar(char c) {
  if (strUsed_ >= kJsonStrPoolMax) return false;
  strPool_[strUsed_++] = c;
  return true;
}

bool JsonDoc::pushUtf8(uint32_t cp) {
  if (cp < 0x80) {
    return pushChar((char)cp);
  } else if (cp < 0x800) {
    return pushChar((char)(0xC0 | (cp >> 6))) &&
           pushChar((char)(0x80 | (cp & 0x3F)));
  } else if (cp < 0x10000) {
    return pushChar((char)(0xE0 | (cp >> 12))) &&
           pushChar((char)(0x80 | ((cp >> 6) & 0x3F))) &&
           pushChar((char)(0x80 | (cp & 0x3F)));
  }
  return pushChar((char)(0xF0 | (cp >> 18))) &&
         pushChar((char)(0x80 | ((cp >> 12) & 0x3F))) &&
         pushChar((char)(0x80 | ((cp >> 6) & 0x3F))) &&
         pushChar((char)(0x80 | (cp & 0x3F)));
}

bool JsonDoc::parseHex4(uint16_t& out) {
  if (end_ - p_ < 4) return false;
  out = 0;
  for (uint8_t i = 0; i < 4; ++i) {
    char c = *p_++;
    uint8_t d;
    if (c >= '0' && c <= '9') d = (uint8_t)(c - '0');
    else if (c >= 'a' && c <= 'f') d = (uint8_t)(c - 'a' + 10);
    else if (c >= 'A' && c <= 'F') d = (uint8_t)(c - 'A' + 10);
    else return false;
    out = (uint16_t)((out << 4) | d);
  }
  return true;
}

// *p_ sits on the opening '"'; fills the pool with decoded contents.
JsonErr JsonDoc::parseStringRaw(const char*& out, uint32_t& outLen) {
  ++p_;
  const char* start = strPool_ + strUsed_;
  while (p_ < end_) {
    unsigned char c = (unsigned char)*p_++;
    if (c == '"') {
      out = start;
      outLen = (uint32_t)(strPool_ + strUsed_ - start);
      return JsonErr::Ok;
    }
    if (c == '\\') {
      if (p_ >= end_) return JsonErr::Malformed;
      char e = *p_++;
      char lit;
      switch (e) {
        case '"': lit = '"'; break;
        case '\\': lit = '\\'; break;
        case '/': lit = '/'; break;
        case 'b': lit = '\b'; break;
        case 'f': lit = '\f'; break;
        case 'n': lit = '\n'; break;
        case 'r': lit = '\r'; break;
        case 't': lit = '\t'; break;
        case 'u': {
          uint16_t w1;
          if (!parseHex4(w1)) return JsonErr::Malformed;
          uint32_t cp;
          if (w1 >= 0xD800 && w1 <= 0xDBFF) {
            // high surrogate: butuh pasangan "\uXXXX" low surrogate
            uint16_t w2 = 0;
            bool paired = (end_ - p_ >= 6 && p_[0] == '\\' && p_[1] == 'u');
            if (paired) {
              p_ += 2;
              paired = parseHex4(w2) && w2 >= 0xDC00 && w2 <= 0xDFFF;
            }
            cp = paired ? (0x10000u + ((uint32_t)(w1 - 0xD800) << 10) +
                           (uint32_t)(w2 - 0xDC00))
                        : 0xFFFD;  // surrogate tak berpasangan -> replacement
          } else if (w1 >= 0xDC00 && w1 <= 0xDFFF) {
            cp = 0xFFFD;  // lone low surrogate
          } else {
            cp = w1;
          }
          if (!pushUtf8(cp)) return JsonErr::StrPoolFull;
          continue;
        }
        default:
          return JsonErr::Malformed;
      }
      if (!pushChar(lit)) return JsonErr::StrPoolFull;
    } else if (c < 0x20) {
      return JsonErr::Malformed;  // raw control chars forbidden in JSON strings
    } else {
      if (!pushChar((char)c)) return JsonErr::StrPoolFull;
    }
  }
  return JsonErr::Malformed;  // unterminated string
}

JsonValue* JsonDoc::parseValue(uint8_t depth) {
  if (depth > kJsonDepthMax) {
    err_ = JsonErr::TooDeep;
    return nullptr;
  }
  skipWs();
  if (p_ >= end_) {
    err_ = JsonErr::Malformed;
    return nullptr;
  }
  char c = *p_;
  if (c == '{') return parseObject(depth);
  if (c == '[') return parseArray(depth);
  if (c == '"') {
    JsonValue* v = allocNode();
    if (!v) return nullptr;
    v->type = JsonType::String;
    err_ = parseStringRaw(v->str, v->strLen);
    return err_ == JsonErr::Ok ? v : nullptr;
  }
  if (c == 't') return parseLiteral(JsonType::Bool, "true", 4, true);
  if (c == 'f') return parseLiteral(JsonType::Bool, "false", 5, false);
  if (c == 'n') return parseLiteral(JsonType::Null, "null", 4, false);
  if (c == '-' || (c >= '0' && c <= '9')) return parseNumber();
  err_ = JsonErr::Malformed;
  return nullptr;
}

JsonValue* JsonDoc::parseLiteral(JsonType t, const char* word, size_t wordLen,
                                 bool b) {
  if ((size_t)(end_ - p_) < wordLen || memcmp(p_, word, wordLen) != 0) {
    err_ = JsonErr::Malformed;
    return nullptr;
  }
  p_ += wordLen;
  JsonValue* v = allocNode();
  if (!v) return nullptr;
  v->type = t;
  v->boolean = b;
  return v;
}

JsonValue* JsonDoc::parseNumber() {
  const char* s = p_;
  while (p_ < end_) {
    char c = *p_;
    if ((c >= '0' && c <= '9') || c == '-' || c == '+' || c == '.' || c == 'e' ||
        c == 'E') {
      ++p_;
    } else {
      break;
    }
  }
  size_t n = (size_t)(p_ - s);
  if (n == 0 || n > 47) {
    err_ = JsonErr::Malformed;
    return nullptr;
  }
  char buf[48];
  memcpy(buf, s, n);
  buf[n] = '\0';
  char* ep = nullptr;
  double d = strtod(buf, &ep);
  if (ep != buf + n) {
    err_ = JsonErr::Malformed;
    return nullptr;
  }
  JsonValue* v = allocNode();
  if (!v) return nullptr;
  v->type = JsonType::Number;
  v->num = d;
  return v;
}

JsonValue* JsonDoc::parseArray(uint8_t depth) {
  ++p_;  // '['
  JsonValue* arr = allocNode();
  if (!arr) return nullptr;
  arr->type = JsonType::Array;
  skipWs();
  if (p_ < end_ && *p_ == ']') {
    ++p_;
    return arr;
  }
  JsonValue* last = nullptr;
  for (;;) {
    JsonValue* v = parseValue(depth + 1);
    if (!v) return nullptr;
    if (last) last->next = v;
    else arr->child = v;
    last = v;
    skipWs();
    if (p_ >= end_) {
      err_ = JsonErr::Malformed;
      return nullptr;
    }
    if (*p_ == ',') {
      ++p_;
      continue;
    }
    if (*p_ == ']') {
      ++p_;
      return arr;
    }
    err_ = JsonErr::Malformed;
    return nullptr;
  }
}

JsonValue* JsonDoc::parseObject(uint8_t depth) {
  ++p_;  // '{'
  JsonValue* obj = allocNode();
  if (!obj) return nullptr;
  obj->type = JsonType::Object;
  skipWs();
  if (p_ < end_ && *p_ == '}') {
    ++p_;
    return obj;
  }
  JsonValue* last = nullptr;
  for (;;) {
    skipWs();
    if (p_ >= end_ || *p_ != '"') {
      err_ = JsonErr::Malformed;
      return nullptr;
    }
    const char* k = nullptr;
    uint32_t kl = 0;
    err_ = parseStringRaw(k, kl);
    if (err_ != JsonErr::Ok) return nullptr;
    skipWs();
    if (p_ >= end_ || *p_ != ':') {
      err_ = JsonErr::Malformed;
      return nullptr;
    }
    ++p_;
    JsonValue* v = parseValue(depth + 1);
    if (!v) return nullptr;
    v->key = k;
    v->keyLen = kl;
    if (last) last->next = v;
    else obj->child = v;
    last = v;
    skipWs();
    if (p_ >= end_) {
      err_ = JsonErr::Malformed;
      return nullptr;
    }
    if (*p_ == ',') {
      ++p_;
      continue;
    }
    if (*p_ == '}') {
      ++p_;
      return obj;
    }
    err_ = JsonErr::Malformed;
    return nullptr;
  }
}

// ---- accessor ----

const JsonValue* jsonMember(const JsonValue* obj, const char* key) {
  if (!obj || obj->type != JsonType::Object || !key) return nullptr;
  size_t kl = strlen(key);
  for (const JsonValue* m = obj->child; m; m = m->next) {
    if (m->keyLen == kl && memcmp(m->key, key, kl) == 0) return m;
  }
  return nullptr;
}

size_t jsonLen(const JsonValue* arr) {
  if (!arr || (arr->type != JsonType::Array && arr->type != JsonType::Object))
    return 0;
  size_t n = 0;
  for (const JsonValue* c = arr->child; c; c = c->next) ++n;
  return n;
}

const JsonValue* jsonAt(const JsonValue* arr, size_t i) {
  if (!arr || (arr->type != JsonType::Array && arr->type != JsonType::Object))
    return nullptr;
  const JsonValue* c = arr->child;
  while (c && i--) c = c->next;
  return c;
}

bool jsonStrEq(const JsonValue* v, const char* s) {
  if (!v || v->type != JsonType::String || !s) return false;
  size_t n = strlen(s);
  return v->strLen == n && memcmp(v->str, s, n) == 0;
}

}  // namespace controlme::llmproto
