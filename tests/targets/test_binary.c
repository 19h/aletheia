#include <stdio.h>
#include <stdlib.h>

int simple_math(int a, int b) {
    return (a * b) + 42;
}

int diamond_cfg(int a) {
    if (a > 5) {
        return a + 1;
    } else {
        return a - 1;
    }
}

int loop_cfg(int n) {
    int sum = 0;
    for (int i = 0; i < n; i++) {
        sum += i;
    }
    return sum;
}

int nested_cfg(int a) {
    if (a > 0) {
        if (a < 10) {
            return 1;
        } else {
            return 2;
        }
    }
    return 3;
}

int main(int argc, char** argv) {
    printf("%d %d %d %d\n", simple_math(1, 2), diamond_cfg(5), loop_cfg(3), nested_cfg(1));
    return 0;
}
