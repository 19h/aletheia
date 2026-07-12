#include <stdio.h>

typedef struct {
    double x;
    double y;
} PairDouble;

typedef struct {
    PairDouble head;
    double tail;
} NestedTripleDouble;

typedef struct {
    float x;
    float y;
    float z;
    float w;
} QuadFloat;

__attribute__((noinline))
double consume_nested_triple(NestedTripleDouble value, double bias) {
    return value.head.x + 2.0 * value.head.y + 3.0 * value.tail + bias;
}

__attribute__((noinline))
double consume_spilled_triple(
    double a0,
    double a1,
    double a2,
    double a3,
    double a4,
    double a5,
    NestedTripleDouble value,
    double tail) {
    return a0 + 2.0 * a1 + 3.0 * a2 + 4.0 * a3 + 5.0 * a4
        + 6.0 * a5 + value.head.x + 2.0 * value.head.y
        + 3.0 * value.tail + 7.0 * tail;
}

__attribute__((noinline))
double consume_spilled_quad_float(
    double a0,
    double a1,
    double a2,
    double a3,
    double a4,
    double a5,
    QuadFloat value,
    double tail) {
    double packed;
    if (tail >= 0.0) {
        packed = (double)value.x + 2.0 * (double)value.y
            + 3.0 * (double)value.z + 4.0 * (double)value.w;
    } else {
        packed = 4.0 * (double)value.w + 3.0 * (double)value.z
            + 2.0 * (double)value.y + (double)value.x;
    }
    return a0 + 2.0 * a1 + 3.0 * a2 + 4.0 * a3 + 5.0 * a4
        + 6.0 * a5 + packed + 7.0 * tail;
}

int main(void) {
    volatile double nested_x = 1.25;
    volatile double nested_y = -2.5;
    volatile double nested_z = 4.0;
    volatile double spilled_x = 0.5;
    volatile double spilled_y = -1.25;
    volatile double spilled_z = 2.0;
    volatile float packed_x = 0.5f;
    volatile float packed_y = -1.25f;
    volatile float packed_z = 2.0f;
    volatile float packed_w = 3.5f;
    const NestedTripleDouble nested = {{nested_x, nested_y}, nested_z};
    const NestedTripleDouble spilled = {{spilled_x, spilled_y}, spilled_z};
    const QuadFloat packed = {packed_x, packed_y, packed_z, packed_w};
    const double first = consume_nested_triple(nested, -0.75);
    const double second = consume_spilled_triple(
        1.0, -2.0, 3.0, -4.0, 5.0, -6.0, spilled, 8.0);
    const double third = consume_spilled_quad_float(
        1.0, -2.0, 3.0, -4.0, 5.0, -6.0, packed, 8.0);
    printf("%.17g %.17g %.17g\n", first, second, third);
    return first == 7.5 && second == 39.0 && third == 53.0 ? 0 : 1;
}
