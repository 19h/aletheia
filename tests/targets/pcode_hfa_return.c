#include <stdio.h>

typedef struct TripleDouble {
    double first;
    double second;
    double third;
} TripleDouble;

typedef struct QuadDouble {
    double first;
    double second;
    double third;
    double fourth;
} QuadDouble;

__attribute__((noinline))
TripleDouble make_triple_double(double value) {
    const TripleDouble result = {
        value + 1.25,
        value * 3.5,
        value - 9.75,
    };
    return result;
}

__attribute__((noinline))
QuadDouble make_quad_double(double value) {
    const QuadDouble result = {
        value + 2.0,
        value - 3.0,
        value * 4.0,
        value / 5.0,
    };
    return result;
}

int main(void) {
    const TripleDouble result = make_triple_double(7.0);
    const QuadDouble quad = make_quad_double(10.0);
    printf("hfa=%.2f,%.2f,%.2f;quad=%.2f,%.2f,%.2f,%.2f\n",
           result.first, result.second, result.third,
           quad.first, quad.second, quad.third, quad.fourth);
    return result.first == 8.25
        && result.second == 24.5
        && result.third == -2.75
        && quad.first == 12.0
        && quad.second == 7.0
        && quad.third == 40.0
        && quad.fourth == 2.0
        ? 0 : 1;
}
