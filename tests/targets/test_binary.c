unsigned long _simple_math() {
    unsigned long tmp_0;

    return _simple_math(a1 - 0x1) + _simple_math(a1 - 0x2);
}

unsigned long _diamond_cfg() {
    int tmp_0;
    unsigned long tmp_1;

    if (tmp_0 <= 0x0) {
        return _diamond_cfg(a1 - 0x1) + _diamond_cfg(a1 - 0x2);
    }
}

unsigned long _loop_cfg() {
    int tmp_0;
    unsigned long tmp_1;

    if (tmp_0 >= 0x0) {
        return _loop_cfg(a1 - 0x1) + _loop_cfg(a1 - 0x2);
    }
}

unsigned long _nested_cfg() {
    int tmp_0;
    unsigned long tmp_1;

    if (tmp_0 > 0x0) {
        return _nested_cfg(a1 - 0x1) + _nested_cfg(a1 - 0x2);
    }
    return _nested_cfg(a1 - 0x1) + _nested_cfg(a1 - 0x2);
}

int _main(int a1, char ** a2, char ** a3) {
    int tmp_5;
    unsigned long tmp_0, tmp_1, tmp_2, tmp_3, tmp_4, tmp_6;

    tmp_0 = _simple_math();
    tmp_0 = _diamond_cfg();
    tmp_0 = _loop_cfg();
    tmp_0 = _nested_cfg();
    *(tmp_1) = tmp_2;
    *(tmp_1) = tmp_3;
    *(tmp_1) = tmp_4;
    *(tmp_1) = tmp_0;
    tmp_5 = _printf("%d %d %d %d\n");
    return 0x0;
    return 0x0;
}

int _printf(char * a1) {
    unsigned long tmp_0;

    tmp_0 = *(_printf_ptr);
    /* indirect branch tmp_0 */
}

