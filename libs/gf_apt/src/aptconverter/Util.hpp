#pragma once
#include <fstream>
#include <filesystem>
#include <stdint.h>
#include <vector>
#include <cstdint>
#include <cstring>
#include <type_traits>
#include <memory>
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#define STRLENGTH(x) (4 * ((((uint32_t)strlen(x) + 1) + 3) / 4))
#define GETALIGN(x) ((4 * ((x + 3) / 4)) - x)
// Align a byte pointer up to 4 bytes.
// (Legacy code used uint32_t casts, which fail to compile cleanly on 64-bit.)
#undef ALIGN
#define ALIGN(x)                                                                 \
  do {                                                                           \
    uintptr_t _p = reinterpret_cast<uintptr_t>(x);                               \
    _p = (_p + 3u) & ~uintptr_t(3u);                                             \
    (x) = reinterpret_cast<uint8_t*>(_p);                                        \
  } while (0)
#define B(x) x ? "true" : "false"
// Convert an in-struct "pointer" that actually stores a 32-bit offset from
// aptbuffer into a real pointer. Works for (const) char*, void*, etc.
#undef add
#define add(x)                                                                                          \
  do {                                                                                                  \
    using _T = std::remove_reference_t<decltype(x)>;                                                    \
    static_assert(std::is_pointer_v<_T>, "add(x) expects x to be a pointer type");                     \
    const uint32_t _off = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(x));                        \
    (x) = reinterpret_cast<_T>(aptbuffer + _off);                                                       \
  } while (0)

// Endian helpers.
static inline uint32_t gf_bswap32(uint32_t v) {
  return (v >> 24) | ((v >> 8) & 0x0000FF00u) | ((v << 8) & 0x00FF0000u) | (v << 24);
}
static inline uint16_t gf_bswap16(uint16_t v) { return uint16_t((v >> 8) | (v << 8)); }
static inline void swapByteOrder(uint32_t& ui) { ui = gf_bswap32(ui); }
static inline void swapByteOrder(uint16_t& ui) { ui = gf_bswap16(ui); }

template <class T>
static inline void fixEndianT(T& v) {
  if constexpr (std::is_same_v<T, uint32_t>) {
    swapByteOrder(v);
  } else if constexpr (std::is_same_v<T, uint16_t>) {
    swapByteOrder(v);
  } else if constexpr (std::is_pointer_v<T>) {
    // Legacy file format stores 32-bit offsets even if T is pointer-typed.
    uint32_t tmp = 0;
    std::memcpy(&tmp, &v, sizeof(uint32_t));
    swapByteOrder(tmp);
    std::memcpy(&v, &tmp, sizeof(uint32_t));
  }
}

inline uint32_t HexToDecimal(const char *str)
{
	return (uint32_t)strtol(str, NULL, 16);
}

//read an integer from memory
inline uint32_t ReadUint(uint8_t *&iter)
{
	uint32_t result = *(uint32_t *)iter;
	result = (result >> 24) |
			 ((result << 8) & 0x00FF0000) |
			 ((result >> 8) & 0x0000FF00) |
			 (result << 24);
	iter += 4;
	return result;
}

template <class T>
inline uint8_t GetByte(T num, uint8_t byte)
{
	uint8_t result;
	switch (byte)
	{
	case 0:
		result = LOBYTE(LOWORD(num));
		break;
	case 1:
		result = HIBYTE(LOWORD(num));
		break;
	case 2:
		result = LOBYTE(HIWORD(num));
		break;
	case 3:
		result = HIBYTE(HIWORD(num));
		break;
	}

	return result;
}

//split a string at the give character
std::vector<std::string> split(std::string str, std::string sep);
