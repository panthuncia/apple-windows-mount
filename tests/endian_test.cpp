// SPDX-License-Identifier: GPL-3.0-or-later
#include <cstdint>
#include <endianness.h>

// An undefined byte order in the original MSVC port swapped AFC's 64-bit
// lengths and operation numbers while leaving its 16/32-bit values intact.
static_assert(htole64(UINT64_C(42)) == UINT64_C(42));
static_assert(le64toh(UINT64_C(42)) == UINT64_C(42));
static_assert(htole32(UINT32_C(42)) == UINT32_C(42));
static_assert(htole16(UINT16_C(42)) == UINT16_C(42));
int main() { return 0; }
