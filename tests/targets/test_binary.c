unsigned long _simple_math() {
    unsigned long tmp_0;

    return tmp_0;
}

unsigned long _diamond_cfg() {
    int tmp_0;
    unsigned long tmp_1;

    if (tmp_0 <= 0x0) {
        return tmp_1;
    }
}

unsigned long _loop_cfg() {
    int tmp_0;
    unsigned long tmp_1;

    if (tmp_0 >= 0x0) {
        return tmp_1;
    }
}

unsigned long _nested_cfg() {
    int tmp_0;
    unsigned long tmp_1;

    if (tmp_0 > 0x0) {
        return tmp_1;
    }
    return tmp_1;
}

extern const int "%d %d %d %d\n";

int _main(int a1, char ** a2, char ** a3) {
    unsigned long tmp_0, tmp_1, tmp_2, tmp_3, tmp_4;

    a1 = _simple_math();
    a1 = _diamond_cfg();
    a1 = _loop_cfg();
    a1 = _nested_cfg();
    *(tmp_0) = tmp_1;
    *(tmp_0) = tmp_2;
    *(tmp_0) = tmp_3;
    *(tmp_0) = a1;
    a1 = _printf("%d %d %d %d\n");
    return tmp_4;
}

int _printf(char * a1) {
    unsigned long tmp_0;

    tmp_0 = *(0x100004000UL);
    /* indirect branch tmp_0 */
}

