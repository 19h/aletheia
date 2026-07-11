#include "portable_c.hpp"

namespace aletheia {

std::string portable_c_runtime_preamble() {
    return R"ALETHEIA_C(/* Aletheia portable-C support contract: GNU C11, LP64 target model. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef __int128 int128_t;
typedef unsigned __int128 uint128_t;

typedef int clockid_t;
typedef struct __aletheia_timespec timespec;
typedef struct __aletheia_FILE FILE;

static inline uint64_t __aletheia_undefined_u64(void) {
    __builtin_trap();
}
static inline uint64_t __aletheia_pow_u64(uint64_t base, uint64_t exponent) {
    uint64_t result = UINT64_C(1);
    while (exponent != 0) {
        if ((exponent & UINT64_C(1)) != 0) result *= base;
        exponent >>= 1;
        if (exponent != 0) base *= base;
    }
    return result;
}
static inline uint64_t __pcode_popcount(uint64_t value, size_t bits) {
    if (bits == 0 || bits > 64) __builtin_trap();
    if (bits < 64) value &= (UINT64_C(1) << bits) - UINT64_C(1);
    return (uint64_t)__builtin_popcountll(value);
}
static inline uint64_t __pcode_lzcount(uint64_t value, size_t bits) {
    if (bits == 0 || bits > 64) __builtin_trap();
    if (bits < 64) value &= (UINT64_C(1) << bits) - UINT64_C(1);
    if (value == 0) return (uint64_t)bits;
    return (uint64_t)__builtin_clzll(value) - (uint64_t)(64 - bits);
}
#define __pcode_float_nan(value) __builtin_isnan(value)
#define __pcode_float_abs(value) __builtin_fabs(value)
#define __pcode_float_sqrt(value) __builtin_sqrt(value)
#define __pcode_ceil(value) __builtin_ceil(value)
#define __pcode_floor(value) __builtin_floor(value)
#define __pcode_round(value) __builtin_round(value)

)ALETHEIA_C";
}

} // namespace aletheia
