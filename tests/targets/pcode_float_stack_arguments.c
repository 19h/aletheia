#include <stdio.h>

__attribute__((noinline))
double weighted_sum9(double a0,
                     double a1,
                     double a2,
                     double a3,
                     double a4,
                     double a5,
                     double a6,
                     double a7,
                     double a8) {
    return a0
        + 2.0 * a1
        + 3.0 * a2
        + 4.0 * a3
        + 5.0 * a4
        + 6.0 * a5
        + 7.0 * a6
        + 8.0 * a7
        + 9.0 * a8;
}

#if defined(__aarch64__) && defined(__APPLE__)
// Force the ninth floating argument to be defined by two predecessor blocks
// and consumed by one joined BL. This shape exercises path-complete outgoing
// stack evidence rather than the compiler's usual store-immediately-before-BL
// sequence.
__attribute__((noinline))
double joined_float_stack_call(int select_first) {
    double result;
    __asm__ volatile(
        "sub sp, sp, #16\n"
        "cbz %w[select], 1f\n"
        "fmov d16, #1.0\n"
        "str d16, [sp]\n"
        "b 2f\n"
        "1:\n"
        "fmov d17, #2.0\n"
        "str d17, [sp]\n"
        "2:\n"
        "fmov d0, xzr\n"
        "fmov d1, xzr\n"
        "fmov d2, xzr\n"
        "fmov d3, xzr\n"
        "fmov d4, xzr\n"
        "fmov d5, xzr\n"
        "fmov d6, xzr\n"
        "fmov d7, xzr\n"
        "bl _weighted_sum9\n"
        "fmov %d[result], d0\n"
        "add sp, sp, #16\n"
        : [result] "=w"(result)
        : [select] "r"(select_first)
        : "x30", "d0", "d1", "d2", "d3", "d4", "d5", "d6", "d7",
          "d16", "d17", "cc", "memory");
    return result;
}
#else
__attribute__((noinline))
double joined_float_stack_call(int select_first) {
    const double ninth = select_first ? 1.0 : 2.0;
    return weighted_sum9(0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, ninth);
}
#endif

int main(void) {
    const double result = weighted_sum9(
        1.0, 2.0, 3.0, 4.0, 5.0, 6.0, 7.0, 8.0, 9.0);
    printf("weighted-float=%.1f\n", result);
    return result == 285.0 ? 0 : 1;
}
