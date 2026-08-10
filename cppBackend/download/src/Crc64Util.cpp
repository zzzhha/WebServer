#include "Crc64Util.h"

#include <cstring>
#include <fstream>

uint64_t Crc64Util::table_[256];
bool Crc64Util::table_initialized_ = false;

void Crc64Util::InitTable() {
  for (uint64_t i = 0; i < 256; ++i) {
    uint64_t crc = i;
    for (int j = 0; j < 8; ++j) {
      if (crc & 1ULL) {
        crc = (crc >> 1) ^ kPoly;
      } else {
        crc >>= 1;
      }
    }
    table_[i] = crc;
  }
  table_initialized_ = true;
}

uint64_t Crc64Util::Compute(const char* data, size_t len) {
  if (!table_initialized_) InitTable();
  uint64_t crc = ~0ULL;
  for (size_t i = 0; i < len; ++i) {
    uint8_t idx = static_cast<uint8_t>(crc) ^ static_cast<uint8_t>(data[i]);
    crc = (crc >> 8) ^ table_[idx];
  }
  return ~crc;
}

uint64_t Crc64Util::ComputeChunkCrc64(const char* data, size_t len) {
  return Compute(data, len);
}

uint64_t Crc64Util::ComputeFileCrc64(const std::string& path) {
  std::ifstream file(path, std::ios::binary);
  if (!file.is_open()) return 0;

  if (!table_initialized_) InitTable();
  uint64_t crc = ~0ULL;

  char buf[64 * 1024];
  while (file) {
    file.read(buf, sizeof(buf));
    std::streamsize n = file.gcount();
    if (n > 0) {
      for (std::streamsize i = 0; i < n; ++i) {
        uint8_t idx = static_cast<uint8_t>(crc) ^ static_cast<uint8_t>(buf[i]);
        crc = (crc >> 8) ^ table_[idx];
      }
    }
  }
  return ~crc;
}

namespace {

// GF(2) 矩阵乘法：vec 二进制位为 1 的列累加 mat 行（zlib gf2_matrix_times）
uint64_t Gf2MatrixTimes(const uint64_t* mat, uint64_t vec) {
  uint64_t sum = 0;
  while (vec) {
    if (vec & 1ULL) sum ^= *mat;
    vec >>= 1;
    ++mat;
  }
  return sum;
}

// GF(2) 矩阵平方（zlib gf2_matrix_square）
void Gf2MatrixSquare(uint64_t* square, const uint64_t* mat) {
  for (int n = 0; n < 64; ++n) {
    square[n] = Gf2MatrixTimes(mat, mat[n]);
  }
}

}  // namespace

// P2 修复：CRC combine 需要 GF(2) 矩阵平方运算（移植 zlib crc32_combine 到 CRC-64）。
// 原实现把"乘以 x 的一列"当作矩阵平方，且循环体重复拷贝 even，导致数学错误。
// 语义：Combine(CRC(A), CRC(B), len(B)) == CRC(A+B)，len_b 为 B 的字节数。
uint64_t Crc64Util::Combine(uint64_t crc_a, uint64_t crc_b, uint64_t len_b) {
  if (len_b == 0) return crc_a;

  uint64_t even[64];
  uint64_t odd[64];
  odd[0] = kPoly;  // 乘以 x^1 的线性变换
  uint64_t row = 1;
  for (int n = 1; n < 64; ++n) {
    odd[n] = row;
    row <<= 1;
  }
  Gf2MatrixSquare(even, odd);  // x^2
  Gf2MatrixSquare(odd, even);  // x^4

  do {
    Gf2MatrixSquare(even, odd);
    if (len_b & 1ULL) crc_a = Gf2MatrixTimes(even, crc_a);
    len_b >>= 1;
    if (len_b == 0) break;
    Gf2MatrixSquare(odd, even);
    if (len_b & 1ULL) crc_a = Gf2MatrixTimes(odd, crc_a);
    len_b >>= 1;
  } while (len_b != 0);

  crc_a ^= crc_b;
  return crc_a;
}

std::string Crc64Util::ToHex(uint64_t crc) {
  static const char* kHex = "0123456789abcdef";
  std::string out(16, '0');
  for (int i = 15; i >= 0; --i) {
    out[i] = kHex[crc & 0x0F];
    crc >>= 4;
  }
  return out;
}