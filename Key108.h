#pragma once
#include <stdint.h>

// Security access algorithm 108. Intermediate values wrap to 16 bits.
constexpr uint16_t swapKeyBytes(uint16_t value) {
  return uint16_t((uint32_t(value)<<8)|(value>>8));
}
constexpr uint16_t corsaEKey108(uint16_t seed) {
  uint16_t value=uint16_t(swapKeyBytes(seed)-0x8F53u);
  value=uint16_t(swapKeyBytes(value)+0x933Eu);
  return uint16_t((value>>7)|(uint32_t(value)<<9));
}
