#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>

typedef struct Triple64 {
    uint64_t first;
    uint64_t second;
    uint64_t third;
} Triple64;

__attribute__((noinline))
Triple64 make_triple64(uint64_t value) {
    const Triple64 result = {
        value + UINT64_C(1),
        value * UINT64_C(3),
        value ^ UINT64_C(0xa5a5a5a5a5a5a5a5),
    };
    return result;
}

int main(void) {
    const Triple64 result = make_triple64(UINT64_C(7));
    printf("triple=%" PRIu64 ",%" PRIu64 ",%" PRIu64 "\n",
           result.first, result.second, result.third);
    return result.first == UINT64_C(8)
        && result.second == UINT64_C(21)
        && result.third == UINT64_C(0xa5a5a5a5a5a5a5a2)
        ? 0 : 1;
}
