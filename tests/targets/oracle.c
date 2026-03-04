unsigned long _analyze_series() {
    int exit_flag_0, tmp_0, tmp_1, tmp_12, tmp_3, tmp_5, tmp_6, tmp_7;
    unsigned int tmp_10, tmp_11, tmp_13, tmp_2, tmp_4, tmp_8, tmp_9;
    unsigned long tmp_14;

    if (arg_0 == 0x0) {
        tmp_0 = arg_1 - 0x1;
        if (arg_1 < 0x1UL) {
            while (true) {
                tmp_1 = tmp_0;
                tmp_2 = tmp_3;
                tmp_4 = tmp_5;
                arg_1 = tmp_6;
                if (*(arg_0) == 0x0) {
                    tmp_7 = arg_3 & 0x3U;
                    tmp_0 = tmp_7 - 0x1;
                    if (tmp_7 > 0x1UL) {
                        if (tmp_7 != 0x0) {
                            tmp_2 += *(arg_0) * arg_1;
                            tmp_3 = tmp_2;
                            tmp_5 = tmp_4;
                        } else {
                            tmp_2 += *(arg_0);
                            tmp_3 = tmp_2;
                            tmp_5 = tmp_4;
                        }
                        tmp_0 = tmp_7 - 0x2;
                        if (tmp_7 == 0x2UL) {
                            tmp_0 = *(arg_0) - arg_2;
                            tmp_2 += (*(arg_0) != arg_2 ? -(*(arg_0)) : *(arg_0));
                            tmp_3 = tmp_2;
                            tmp_5 = tmp_4;
                        } else if ((*(arg_0) & 0x1) != 0x0) {
                            if (*(arg_0) + tmp_8 + tmp_2 <= tmp_9) {
                                exit_flag_0 = 0x0;
                                break;
                            }
                            tmp_10 = *(arg_0) + tmp_8 + tmp_2;
                            tmp_0 = *(arg_0) + tmp_8 + tmp_2 - tmp_9;
                            tmp_3 = tmp_10;
                            tmp_5 = tmp_4;
                        } else {
                            tmp_2 -= tmp_11;
                            tmp_3 = tmp_2;
                            tmp_5 = tmp_4;
                        }
                    }
                    tmp_0 = tmp_1;
                    tmp_3 = tmp_2;
                    tmp_5 = tmp_4;
                }
                tmp_12 = tmp_0;
                tmp_1 = tmp_3;
                tmp_6 = tmp_5;
                arg_1++;
                if (tmp_12 == 0x0) {
                    tmp_0 = tmp_12;
                    tmp_3 = tmp_1;
                    tmp_5 = tmp_6;
                    tmp_6 = arg_1;
                    continue;
                }
                exit_flag_0 = 0x1;
                break;
            }
            if (exit_flag_0 == 0x0) {
                tmp_3 = tmp_10;
                tmp_5 = tmp_4;
            } else {
                tmp_3 = tmp_1;
                tmp_5 = tmp_6;
            }
            tmp_2 = tmp_3;
            tmp_6 = tmp_5;
            tmp_7 = (arg_2 != 0x0 ? arg_2 : tmp_13);
            tmp_2 += tmp_7;
            tmp_3 = tmp_2;
            while (true) {
                if (tmp_2 <= 0x3e8UL) {
                    break;
                }
                tmp_2 = tmp_3;
                tmp_2 -= tmp_7;
                tmp_3 = tmp_2;
            }
            tmp_3 = tmp_2;
            tmp_5 = tmp_6;
            do {
                tmp_2 = tmp_3;
                tmp_4 = tmp_5;
                tmp_2 += tmp_4;
                tmp_4 >>= 0x1UL;
            } while (((tmp_4 == 0x1UL ? 0x0UL : tmp_2 & 0x1U)) != 0x0);
            break;
            tmp_3 = tmp_2;
            tmp_5 = tmp_4;
            continue;
            return tmp_14;
        }
        return tmp_14;
    }
}

unsigned long _fold_bytes() {
    int tmp_0;
    unsigned long tmp_1;

    if (arg_0 == 0x0) {
        if (arg_1 == 0x0) {
            while (true) {
                if (arg_1 == arg_0) {
                    break;
                }
                arg_0 = tmp_0;
                arg_0++;
                tmp_0 = arg_0;
            }
            if (arg_1 != arg_0) {
                return tmp_1;
            }
            return tmp_1;
        }
    }
    return tmp_1;
}

unsigned long _control_flow_showcase() {
    int exit_flag_0, tmp_0, tmp_11, tmp_12, tmp_13, tmp_16, tmp_3, tmp_4, tmp_5, tmp_6, tmp_9;
    unsigned int tmp_1, tmp_10, tmp_14, tmp_15, tmp_2, tmp_7, tmp_8;
    unsigned long tmp_17;

    tmp_0 = arg_0 - 0xc9;
    tmp_1 = (tmp_0 ^ tmp_2) & 0x3U;
    tmp_3 = tmp_4;
    tmp_4 = tmp_1;
    tmp_5 = tmp_6;
    tmp_7 = tmp_3;
    tmp_1 = tmp_4;
    tmp_8 = tmp_9;
    tmp_10 = tmp_11;
    arg_0 = tmp_5;
    switch (tmp_1) {
        case 1:
            if (arg_0 > 0x32UL) {
                exit_flag_0 = 0x1;
            }
            if (tmp_1 != 0x0) {
                arg_0 = tmp_2 + arg_0 + tmp_8;
                if ((arg_0 & 0x1) != 0x0) {
                    if (tmp_12 == arg_0) {
                        exit_flag_0 = 0x0;
                        break;
                    }
                    tmp_4 = tmp_13;
                    tmp_5 = arg_0;
                    arg_0 += (tmp_0 == tmp_2 ? -(tmp_2) : tmp_0);
                    tmp_1 = arg_0 >> 0x1fUL & 0x3U;
                    tmp_4 = tmp_1;
                    tmp_5 = arg_0;
                    tmp_12 = tmp_0 + tmp_2 + tmp_10;
                    arg_0 ^= tmp_12;
                    tmp_1 = ((arg_0 & 0x80000000U) == 0x0 ? tmp_14 & ~(tmp_7) : tmp_15);
                    tmp_4 = tmp_1;
                    tmp_5 = arg_0;
                    tmp_4 = tmp_16;
                    tmp_5 = arg_0;
                }
            }
            break;
        case 2:
            arg_0 += ((tmp_0 & tmp_2) == 0x0 ? tmp_10 : tmp_8);
            tmp_1 = (((arg_0 + 0x14UL == 0x0 ? 0x0UL : tmp_10 - 0x8UL)) != 0x0 ? tmp_14 + 0x1 : tmp_14);
            tmp_4 = tmp_1;
            tmp_5 = arg_0;
            tmp_11 = tmp_4;
            arg_0 = tmp_5;
            tmp_4 = tmp_11;
            tmp_5 = arg_0;
            tmp_11 = tmp_4;
            tmp_6 = tmp_5;
            tmp_10++;
            tmp_8--;
            tmp_7 += 0x2;
            if (tmp_10 != 0xcUL) {
                tmp_3 = tmp_7;
                tmp_4 = tmp_11;
                tmp_9 = tmp_8;
                tmp_11 = tmp_10;
                tmp_5 = tmp_6;
                continue;
            }
            break;
    }
    exit_flag_0 = 0x1;
    if (exit_flag_0 != 0x0) {
        return tmp_17;
    }
    return tmp_17;
}

