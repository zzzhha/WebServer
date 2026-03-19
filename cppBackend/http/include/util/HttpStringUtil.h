#pragma once

#include <string>
#include <string_view>

inline char ToLowerAscii(char c) {
  if (c >= 'A' && c <= 'Z') return static_cast<char>(c - 'A' + 'a');
  return c;
}

inline void LowerAsciiInPlace(std::string& s) {
  for (char& c : s) c = ToLowerAscii(c);
}

inline std::string LowerAsciiCopy(std::string_view s) {
  std::string out;
  out.reserve(s.size());
  for (char c : s) out.push_back(ToLowerAscii(c));
  return out;
}

inline std::string_view TrimAsciiWhitespace(std::string_view s) {
  size_t start = 0;
  while (start < s.size()) {
    char c = s[start];
    if (c != ' ' && c != '\t' && c != '\r' && c != '\n') break;
    ++start;
  }
  size_t end = s.size();
  while (end > start) {
    char c = s[end - 1];
    if (c != ' ' && c != '\t' && c != '\r' && c != '\n') break;
    --end;
  }
  return s.substr(start, end - start);
}
