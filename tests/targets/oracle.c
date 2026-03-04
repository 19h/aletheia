unsigned long _analyze_series(unsigned long arg_0, unsigned long arg_1, unsigned long arg_2, unsigned long arg_3) {
    int exit_flag_0, tmp_1, tmp_12, tmp_2, tmp_4, tmp_6, tmp_7, tmp_8;
    unsigned int tmp_10, tmp_11, tmp_3, tmp_5, tmp_9;
    unsigned long tmp_0;

    if (arg_0 == 0x0) {
        return tmp_0;
    } else {
        tmp_1 = arg_1 - 0x1;
        if (arg_1 < 0x1UL) {
            return tmp_0;
        }
    }
    tmp_2 = tmp_1;
    while (true) {
        tmp_3 = tmp_4;
        tmp_5 = tmp_6;
        arg_1 = tmp_7;
        tmp_1 = tmp_2;
        if (*(arg_0) == 0x0) {
            if ((*(arg_0) & 0x80000000) != 0x0) {
                tmp_5++;
                tmp_3 -= *(arg_0);
                tmp_4 = tmp_3;
                tmp_6 = tmp_5;
                tmp_2 = tmp_1;
                tmp_8 = arg_3 & 0x3U;
                tmp_1 = tmp_8 - 0x1;
                if (tmp_8 > 0x1UL) {
                    if (tmp_8 != 0x0) {
                        tmp_3 += *(arg_0) * arg_1;
                        tmp_4 = tmp_3;
                        tmp_6 = tmp_5;
                        tmp_2 = tmp_1;
                    } else {
                        tmp_3 += *(arg_0);
                        tmp_4 = tmp_3;
                        tmp_6 = tmp_5;
                        tmp_2 = tmp_1;
                    }
                    tmp_1 = tmp_8 - 0x2;
                    if (tmp_8 == 0x2UL) {
                        tmp_1 = *(arg_0) - arg_2;
                        tmp_3 += (*(arg_0) != arg_2 ? -(*(arg_0)) : *(arg_0));
                        tmp_4 = tmp_3;
                        tmp_6 = tmp_5;
                        tmp_2 = tmp_1;
                    } else if ((*(arg_0) & 0x1) != 0x0) {
                        if (*(arg_0) + tmp_9 + tmp_3 <= 0x1770U) {
                            exit_flag_0 = 0x0;
                            break;
                        }
                        tmp_10 = *(arg_0) + tmp_9 + tmp_3;
                        tmp_1 = *(arg_0) + tmp_9 + tmp_3 - 0x1770U;
                        tmp_4 = tmp_10;
                        tmp_6 = tmp_5;
                        tmp_2 = tmp_1;
                    } else {
                        tmp_3 -= tmp_11;
                        tmp_4 = tmp_3;
                        tmp_6 = tmp_5;
                        tmp_2 = tmp_1;
                    }
                }
            }
            tmp_4 = tmp_3;
            tmp_6 = tmp_5;
            tmp_2 = tmp_1;
        }
        tmp_12 = tmp_4;
        tmp_7 = tmp_6;
        tmp_1 = tmp_2;
        arg_1++;
        if (tmp_1 == 0x0) {
            tmp_4 = tmp_12;
            tmp_6 = tmp_7;
            tmp_7 = arg_1;
            tmp_2 = tmp_1;
            continue;
        }
        exit_flag_0 = 0x1;
        break;
    }
    if (exit_flag_0 == 0x0) {
        tmp_4 = tmp_10;
        tmp_6 = tmp_5;
    } else {
        tmp_4 = tmp_12;
        tmp_6 = tmp_7;
    }
    tmp_3 = tmp_4;
    tmp_7 = tmp_6;
    tmp_8 = (arg_2 == 0x0 ? 0x7U : arg_2);
    tmp_3 += tmp_8;
    tmp_4 = tmp_3;
    while (true) {
        if (tmp_3 <= 0x3e8UL) {
            break;
        }
        tmp_3 = tmp_4;
        tmp_3 -= tmp_8;
        tmp_4 = tmp_3;
    }
    tmp_4 = tmp_3;
    tmp_6 = tmp_7;
    do {
        tmp_3 = tmp_4;
        tmp_5 = tmp_6;
        tmp_3 += tmp_5;
        tmp_5 >>= 0x1UL;
    } while (((tmp_5 == 0x1UL ? 0x0UL : tmp_3 & 0x1U)) != 0x0);
    break;
    tmp_4 = tmp_3;
    tmp_6 = tmp_5;
    continue;
    return tmp_0;
}

unsigned long _fold_bytes(unsigned long arg_0, unsigned long arg_1) {
    int tmp_0, tmp_1, tmp_11, tmp_12, tmp_4, tmp_7, tmp_8, tmp_9;
    unsigned int tmp_10, tmp_3, tmp_5, tmp_6;
    unsigned long tmp_13, tmp_14, tmp_2;

    if (arg_0 == 0x0) {
        if (arg_1 == 0x0) {
            arg_1 ^= 0xfffffff8;
            tmp_0 = tmp_1;
            tmp_1 = arg_1;
            while (true) {
                if (arg_1 == tmp_2) {
                    break;
                }
                tmp_2 = tmp_0;
                arg_1 = tmp_1;
                tmp_3 = *(arg_0);
                tmp_4 = ((*(arg_0) & 0x3U) != 0x1UL ? ((*(arg_0) & 0x3U) == 0x2UL ? *(arg_0) + tmp_5 + tmp_6 ^ arg_1 : arg_1 - (*(arg_0) << 0x5)) : ((*(arg_0) & 0x3U) != 0x0 ? arg_1 + *(arg_0) * 0x45d9f3bU : __ROR__(arg_1 + *(arg_0), 0xfffffff8) ^ arg_1));
                tmp_7 = tmp_4 ^ 0xa5a5a5a5U;
                tmp_8 = ((*(arg_0) & 0x3U) != 0x1UL ? ((*(arg_0) & 0x3U) == 0x2UL ? *(arg_0) + tmp_5 + tmp_6 ^ arg_1 : arg_1 - (*(arg_0) << 0x5)) : ((*(arg_0) & 0x3U) != 0x0 ? arg_1 + *(arg_0) * 0x45d9f3bU : __ROR__(arg_1 + *(arg_0), 0xfffffff8) ^ arg_1));
                tmp_9 = tmp_8 ^ 0xa5a5a5a5U;
                tmp_10 = tmp_11(tmp_7, tmp_9, 0x1bUL);
                tmp_12 = tmp_3 & 0x3U;
                arg_1 = ((tmp_13 & 0xfUL) == 0x0 ? (tmp_12 != 0x1UL ? (tmp_12 == 0x2UL ? tmp_3 + tmp_5 + tmp_6 ^ arg_1 : arg_1 - (tmp_3 << 0x5)) : (tmp_12 != 0x0 ? arg_1 + tmp_3 * 0x45d9f3bU : __ROR__(arg_1 + tmp_3, 0xfffffff8) ^ arg_1)) : tmp_11((((*(arg_0) & 0x3U) != 0x1UL ? ((*(arg_0) & 0x3U) == 0x2UL ? *(arg_0) + tmp_5 + tmp_6 ^ arg_1 : arg_1 - (*(arg_0) << 0x5)) : ((*(arg_0) & 0x3U) != 0x0 ? arg_1 + *(arg_0) * 0x45d9f3bU : __ROR__(arg_1 + *(arg_0), 0xfffffff8) ^ arg_1))) ^ 0xa5a5a5a5U, (((*(arg_0) & 0x3U) != 0x1UL ? ((*(arg_0) & 0x3U) == 0x2UL ? *(arg_0) + tmp_5 + tmp_6 ^ arg_1 : arg_1 - (*(arg_0) << 0x5)) : ((*(arg_0) & 0x3U) != 0x0 ? arg_1 + *(arg_0) * 0x45d9f3bU : __ROR__(arg_1 + *(arg_0), 0xfffffff8) ^ arg_1))) ^ 0xa5a5a5a5U, 0x1bUL));
                tmp_2++;
                tmp_0 = tmp_2;
                tmp_1 = arg_1;
            }
            if (arg_1 == tmp_2) {
                return tmp_14;
            }
        }
    }
    return tmp_14;
}

unsigned long _control_flow_showcase(unsigned long arg_0) {
    int exit_flag_0, tmp_0, tmp_10, tmp_11, tmp_12, tmp_13, tmp_2, tmp_3, tmp_4, tmp_5, tmp_8;
    unsigned int tmp_1, tmp_6, tmp_7, tmp_9;
    unsigned long tmp_14;

    tmp_0 = arg_0 - 0xc9;
    tmp_1 = (tmp_0 ^ 0xfffffff7U) & 0x3U;
    tmp_2 = tmp_3;
    tmp_3 = tmp_1;
    tmp_4 = tmp_5;
    while (true) {
        tmp_6 = tmp_2;
        tmp_1 = tmp_3;
        tmp_7 = tmp_8;
        tmp_9 = tmp_10;
        arg_0 = tmp_4;
        switch (tmp_1) {
            case 1:
                if (arg_0 > 0x32UL) {
                    exit_flag_0 = 0x3;
                    break;
                }
                if (tmp_1 != 0x0) {
                    arg_0 = arg_0 + 0xfffffff7U + tmp_7;
                    if ((arg_0 & 0x1) != 0x0) {
                        if (tmp_11 == arg_0) {
                            exit_flag_0 = 0x2;
                            break;
                        }
                        tmp_3 = tmp_12;
                        tmp_4 = arg_0;
                        arg_0 += (tmp_0 == 0xfffffff7U ? -(0xfffffff7U) : tmp_0);
                        tmp_1 = arg_0 >> 0x1fUL & 0x3U;
                        tmp_3 = tmp_1;
                        tmp_4 = arg_0;
                        tmp_11 = tmp_0 + 0xfffffff7U + tmp_9;
                        arg_0 ^= tmp_11;
                        tmp_1 = ((arg_0 & 0x80000000U) != 0x0 ? 0x1U : ~(tmp_6) & 0x2U);
                        tmp_3 = tmp_1;
                        tmp_4 = arg_0;
                        tmp_3 = tmp_13;
                        tmp_4 = arg_0;
                    }
                }
                break;
            case 2:
                if (tmp_0 + 0x191UL < 0x0) {
                    exit_flag_0 = 0x1;
                    break;
                }
                arg_0 += ((tmp_0 & 0xfffffff7U) == 0x0 ? tmp_9 : tmp_7);
                tmp_1 = (((arg_0 + 0x14UL == 0x0 ? 0x0UL : tmp_9 - 0x8UL)) == 0x0 ? 0x2U : 0x3);
                tmp_3 = tmp_1;
                tmp_4 = arg_0;
                tmp_10 = tmp_3;
                arg_0 = tmp_4;
                tmp_3 = tmp_10;
                tmp_4 = arg_0;
                break;
        }
        tmp_10 = tmp_3;
        tmp_5 = tmp_4;
        tmp_9++;
        tmp_7--;
        tmp_6 += 0x2;
        if (tmp_9 != 0xcUL) {
            tmp_2 = tmp_6;
            tmp_3 = tmp_10;
            tmp_8 = tmp_7;
            tmp_10 = tmp_9;
            tmp_4 = tmp_5;
            continue;
        }
        exit_flag_0 = 0x0;
        break;
    }
    if (exit_flag_0 == 0x0) {
        tmp_4 = tmp_5;
        arg_0 = tmp_4;
        if ((arg_0 & 0x80000000) == 0x0) {
            return tmp_14;
        }
    }
    if (exit_flag_0 == 0x1) {
        tmp_4 = arg_0;
    }
    return tmp_14;
}

