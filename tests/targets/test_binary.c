long _simple_math() {
    int ret_0;
    unsigned int local_m16, tmp_1, tmp_2;
    unsigned long tmp_0;

    tmp_0 -= 0x10;
    local_m16 = arg_0;
    tmp_1 = arg_1;
    tmp_2 = arg_1;
    tmp_1 *= tmp_2;
    arg_0 = tmp_1 + 0x2a;
    tmp_0 += 0x10UL;
    return ret_0;
}

long _diamond_cfg() {
    int ret_0, tmp_2;
    unsigned int local_m16, tmp_1;
    unsigned long tmp_0;

    tmp_0 -= 0x10;
    local_m16 = arg_0;
    tmp_1 -= 0x5UL;
    if (tmp_2 <= 0x0) {
        tmp_1--;
        arg_0 = tmp_1;
        tmp_0 += 0x10;
        return ret_0;
    } else {
        tmp_1++;
        local_m16 = tmp_1;
        arg_0 = tmp_1;
        tmp_0 += 0x10;
        return ret_0;
    }
}

long _loop_cfg() {
    int ret_0, tmp_3;
    unsigned int local_m16, tmp_1, tmp_2, wzr;
    unsigned long tmp_0;

    tmp_0 -= 0x10;
    local_m16 = arg_0;
    local_m16 = wzr;
    local_m16 = wzr;
    tmp_1 = tmp_2;
    tmp_2 -= tmp_1;
    if (tmp_3 >= 0x0) {
        arg_0 = tmp_2;
        tmp_0 += 0x10;
        return ret_0;
    } else {
        tmp_1 = tmp_2;
        tmp_2 += tmp_1;
        local_m16 = tmp_2;
        tmp_2++;
        /* loop -> bb_1 */
    }
}

long _nested_cfg() {
    int ret_0, tmp_1;
    unsigned int local_m16, tmp_2;
    unsigned long tmp_0;

    tmp_0 -= 0x10;
    local_m16 = arg_0;
    if (tmp_1 <= 0x0) {
        tmp_2 = 0x3U;
        arg_0 = tmp_2;
        tmp_0 += 0x10;
        return ret_0;
    } else {
        tmp_2 -= 0xaUL;
        if (tmp_1 >= 0x0) {
            tmp_2 = 0x2U;
            local_m16 = tmp_2;
            arg_0 = tmp_2;
            tmp_0 += 0x10;
            return ret_0;
        } else {
            tmp_2 = 0x1U;
            local_m16 = tmp_2;
            arg_0 = tmp_2;
            tmp_0 += 0x10;
            return ret_0;
        }
    }
}

int _main(int a1, char ** a2, char ** a3) {
    unsigned int local_0, ret_0, tmp_2, tmp_3, tmp_6, tmp_7, tmp_8, wzr;
    unsigned long tmp_0, tmp_1, tmp_10, tmp_11, tmp_12, tmp_4, tmp_5, tmp_9;

    tmp_0 -= 0x60;
    tmp_1 = tmp_0 + 0x50UL;
    tmp_2 = 0x0U;
    local_0 = tmp_2;
    local_0 = wzr;
    local_0 = a1;
    local_0 = a2;
    a1 = 0x1U;
    local_0 = a1;
    tmp_3 = 0x2U;
    tmp_4 = _simple_math();
    local_0 = a1;
    a1 = 0x5U;
    tmp_4 = _diamond_cfg();
    local_0 = a1;
    a1 = 0x3U;
    tmp_4 = _loop_cfg();
    tmp_5 = tmp_4;
    a1 = tmp_2;
    tmp_4 = _nested_cfg();
    tmp_6 = tmp_2;
    tmp_7 = tmp_2;
    tmp_8 = tmp_2;
    tmp_9 = tmp_0;
    tmp_5 = tmp_10;
    *(tmp_9) = tmp_5;
    tmp_5 = tmp_11;
    *(tmp_9) = tmp_5;
    tmp_5 = tmp_12;
    *(tmp_9) = tmp_5;
    tmp_5 = tmp_4;
    *(tmp_9) = tmp_5;
    tmp_4 = 0x100000628UL;
    tmp_4 = _printf(tmp_4);
    a1 = tmp_2;
    tmp_0 += 0x60UL;
    return ret_0;
}

int _printf(char * a1) {
    unsigned long tmp_0;

    tmp_0 = 0x100004000UL;
    tmp_0 = *(tmp_0);
    /* indirect branch tmp_0 */
}

