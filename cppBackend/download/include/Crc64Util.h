#pragma once

#include <cstdint>
#include <cstddef>
#include <string>

class Crc64Util {
 public:
  static uint64_t ComputeFileCrc64(const std::string& path);

  static uint64_t Compute(const char* data, size_t len);

  static uint64_t ComputeChunkCrc64(const char* data, size_t len);

  static uint64_t Combine(uint64_t crc_a, uint64_t crc_b, uint64_t len_b);

  static std::string ToHex(uint64_t crc);

 private:
  static const uint64_t kPoly = 0xC96C5795D7870F42ULL;
  static uint64_t table_[256];
  static bool table_initialized_;
  static void InitTable();
};