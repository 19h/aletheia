#include <stdint.h>

static uint64_t relocation_target = UINT64_C(0x0123456789abcdef);
static uint64_t *volatile relocation_pointer = &relocation_target;

static uint64_t read_relocated_value(void) {
    return *relocation_pointer;
}

int main(void) {
    relocation_target += UINT64_C(0x1111111111111111);
    return read_relocated_value() == UINT64_C(0x123456789abcdf00) ? 0 : 1;
}
