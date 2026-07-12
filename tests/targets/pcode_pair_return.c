#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>

typedef struct Pair64 {
    uint64_t first;
    uint64_t second;
} Pair64;

__attribute__((noinline))
Pair64 make_pair64(uint64_t value) {
    const Pair64 result = {
        value + UINT64_C(1),
        value * UINT64_C(3),
    };
    return result;
}

int main(void) {
    const Pair64 result = make_pair64(UINT64_C(7));
    printf("pair=%" PRIu64 ",%" PRIu64 "\n", result.first, result.second);
    return result.first == UINT64_C(8) && result.second == UINT64_C(21) ? 0 : 1;
}
