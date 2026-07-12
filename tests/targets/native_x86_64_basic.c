__attribute__((noinline))
unsigned long combine4(unsigned long a0,
                       unsigned long a1,
                       unsigned long a2,
                       unsigned long a3) {
    return a0 + 3UL * a1 + 5UL * a2 + 7UL * a3;
}

int main(void) {
    return combine4(2UL, 3UL, 5UL, 7UL) == 85UL ? 0 : 1;
}
