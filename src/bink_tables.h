#pragma once

#include <cstdint>

namespace nolf::formats
{

extern const uint8_t kBinkScan[64];
extern const uint8_t kTreeBits[16][16];
extern const uint8_t kTreeLens[16][16];
extern const uint8_t kRLERunLens[4];
extern const uint8_t kPatterns[16][64];
extern const int32_t kBinkIntraQuant[16][64];
extern const int32_t kBinkInterQuant[16][64];

} // namespace nolf::formats
