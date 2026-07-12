#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>

__attribute__((noinline))
uint64_t weighted_sum10(uint64_t a0,
                        uint64_t a1,
                        uint64_t a2,
                        uint64_t a3,
                        uint64_t a4,
                        uint64_t a5,
                        uint64_t a6,
                        uint64_t a7,
                        uint64_t a8,
                        uint64_t a9) {
    return a0
        + UINT64_C(3) * a1
        + UINT64_C(5) * a2
        + UINT64_C(7) * a3
        + UINT64_C(11) * a4
        + UINT64_C(13) * a5
        + UINT64_C(17) * a6
        + UINT64_C(19) * a7
        + UINT64_C(23) * a8
        + UINT64_C(29) * a9;
}

int main(void) {
    const uint64_t result = weighted_sum10(
        UINT64_C(2), UINT64_C(3), UINT64_C(5), UINT64_C(7), UINT64_C(11),
        UINT64_C(13), UINT64_C(17), UINT64_C(19), UINT64_C(23), UINT64_C(29));
    printf("weighted=%" PRIu64 "\n", result);
    return result == UINT64_C(2395) ? 0 : 1;
}
