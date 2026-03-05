unsigned long _analyze_series(unsigned long arg_0, unsigned long arg_1, unsigned long arg_2, unsigned long arg_3) {
    int exit_flag_0, tmp_0, tmp_10, tmp_11, tmp_12, tmp_13, tmp_14, tmp_15, tmp_16, tmp_4, tmp_5, tmp_6, tmp_7, tmp_9;
    unsigned int tmp_1, tmp_17, tmp_18, tmp_19, tmp_2, tmp_20, tmp_21, tmp_3, tmp_8;

    if (arg_0 != 0x0) {
        tmp_0 = arg_1 - 0x1;
        if (arg_1 < 0x1UL) {
            return arg_0;
        }
    }
    return arg_0;
    tmp_1 = 0x0U;
    arg_1 = 0x0U;
    arg_3 &= 0x3U;
    tmp_2 = 0x1U;
    tmp_3 = 0x1770U;
    tmp_4 = tmp_1;
    tmp_5 = arg_1;
    tmp_6 = tmp_2;
    tmp_7 = tmp_0;
    while (true) {
        tmp_1 = tmp_4;
        arg_1 = tmp_5;
        tmp_2 = tmp_6;
        tmp_6 = tmp_7;
        if ((*(arg_0) & 0x80000000) != 0x0) {
            arg_1++;
            tmp_1 -= *(arg_0);
            tmp_4 = tmp_1;
            tmp_5 = arg_1;
            tmp_7 = tmp_6;
        } else {
            if (*(arg_0) + tmp_8 + tmp_1 <= tmp_3) {
                tmp_9 = tmp_4;
                tmp_10 = tmp_5;
                tmp_0 = tmp_7;
                tmp_2++;
                if ( == 0x0) {
                    tmp_10 = tmp_5;
                    tmp_15 = (arg_2 == 0x0 ? 0x7U : arg_2);
                    tmp_16 = tmp_15;
                    tmp_1 = tmp_15 + ;
                    tmp_15 =  - tmp_16;
                    tmp_18 = tmp_15;
                    tmp_16 =  + ;
                    tmp_19 = tmp_16;
                    tmp_15 =  >> 0x1UL;
                    tmp_20 = tmp_15;
                    return arg_0;
                }
                tmp_4 = tmp_9;
                tmp_5 = tmp_10;
                tmp_6 = tmp_2;
                tmp_7 = tmp_0;
                continue;
                exit_flag_0 = 0x1;
                break;
            }
            tmp_11 = arg_3 - 0x1;
            if (arg_3 > 0x1UL) {
                tmp_10 = tmp_5;
                tmp_15 = (arg_2 == 0x0 ? 0x7U : arg_2);
                tmp_16 = tmp_15;
                tmp_1 = tmp_15 + ;
                tmp_15 =  - tmp_16;
                tmp_18 = tmp_15;
                tmp_16 =  + ;
                tmp_19 = tmp_16;
                tmp_15 =  >> 0x1UL;
                tmp_20 = tmp_15;
                return arg_0;
            }
            tmp_13 = arg_3 - 0x2;
            if (arg_3 != 0x2UL) {
                tmp_9 = tmp_4;
                tmp_10 = tmp_5;
                tmp_0 = tmp_7;
                tmp_2++;
                tmp_4 = tmp_9;
                tmp_5 = tmp_10;
                tmp_6 = tmp_2;
                tmp_7 = tmp_0;
                continue;
                exit_flag_0 = 0x1;
                break;
            }
            if ((*(arg_0) & 0x1) == 0x0) {
                tmp_1 -= tmp_21;
                tmp_4 = tmp_1;
                tmp_5 = arg_1;
                tmp_7 = tmp_13;
            }
            tmp_17 = *(arg_0) + tmp_8 + tmp_1;
            tmp_14 = *(arg_0) + tmp_8 + tmp_1 - tmp_3;
            exit_flag_0 = 0x0;
            break;
            tmp_4 = tmp_17;
            tmp_5 = arg_1;
            tmp_7 = tmp_14;
            tmp_12 = *(arg_0) - arg_2;
            tmp_1 += (*(arg_0) != arg_2 ? -(*(arg_0)) : *(arg_0));
            tmp_4 = tmp_1;
            tmp_5 = arg_1;
            tmp_7 = tmp_12;
            if (arg_3 != 0x0) {
                tmp_1 += *(arg_0) * tmp_2;
                tmp_4 = tmp_1;
                tmp_5 = arg_1;
                tmp_7 = tmp_11;
            } else {
                tmp_1 += *(arg_0);
                tmp_4 = tmp_1;
                tmp_5 = arg_1;
                tmp_7 = tmp_11;
            }
        }
        tmp_4 = tmp_1;
        tmp_5 = arg_1;
        tmp_7 = tmp_6;
        tmp_9 = tmp_4;
        tmp_10 = tmp_5;
        tmp_0 = tmp_7;
        tmp_2++;
        tmp_4 = tmp_9;
        tmp_5 = tmp_10;
        tmp_6 = tmp_2;
        tmp_7 = tmp_0;
        continue;
        exit_flag_0 = 0x1;
        break;
    }
    if (exit_flag_0 == 0x0) {
        tmp_5 = arg_1;
    } else {
        tmp_5 = tmp_10;
    }
    tmp_10 = tmp_5;
    tmp_15 = (arg_2 == 0x0 ? 0x7U : arg_2);
    tmp_16 = tmp_15;
    tmp_1 = tmp_15 + ;
    tmp_15 =  - tmp_16;
    tmp_18 = tmp_15;
    tmp_16 =  + ;
    tmp_19 = tmp_16;
    tmp_15 =  >> 0x1UL;
    tmp_20 = tmp_15;
    return arg_0;
}

unsigned long _fold_bytes(unsigned long arg_0, unsigned long arg_1, unsigned long arg_2) {
    int tmp_12, tmp_13, tmp_14, tmp_15, tmp_17, tmp_5, tmp_7, tmp_8;
    unsigned int tmp_1, tmp_10, tmp_11, tmp_2, tmp_3, tmp_4, tmp_6;
    unsigned long tmp_0, tmp_16, tmp_18, tmp_9;

    if (arg_1 == 0x0) {
        return arg_0;
    }
    tmp_0 = 0x0UL;
    arg_1 ^= arg_2;
    tmp_1 = 0x5U;
    tmp_2 = 0x45d9f3bU;
    tmp_3 = 0xfffffffdU;
    tmp_4 = 0xa5a5a5a5U;
    tmp_5 = tmp_3 - (tmp_6 & 0x7U);
    arg_2 = tmp_5;
    tmp_7 = tmp_6 & tmp_1;
    tmp_8 = ((*(tmp_9) & 0x3U) != 0x1UL ? ((*(tmp_9) & 0x3U) == 0x2UL ? *(tmp_9) + tmp_10 + tmp_11 ^  :  - (*(tmp_9) << tmp_7)) : ((*(tmp_9) & 0x3U) != 0x0 ?  + *(tmp_9) * tmp_2 : __ROR__( + *(tmp_9), tmp_5) ^ ));
    tmp_12 = tmp_8 ^ tmp_4;
    tmp_13 = ((*(tmp_9) & 0x3U) != 0x1UL ? ((*(tmp_9) & 0x3U) == 0x2UL ? *(tmp_9) + tmp_10 + tmp_11 ^  :  - (*(tmp_9) << tmp_7)) : ((*(tmp_9) & 0x3U) != 0x0 ?  + *(tmp_9) * tmp_2 : __ROR__( + *(arg_0), tmp_5) ^ ));
    tmp_14 = tmp_13 ^ tmp_4;
    arg_2 = tmp_15(tmp_12, tmp_14, 0x1bUL);
    arg_0 = ((tmp_16 & 0xfUL) == 0x0 ? ((*(arg_0) & 0x3U) != 0x1UL ? ((*(arg_0) & 0x3U) == 0x2UL ? *(arg_0) + tmp_10 + tmp_11 ^  :  - (*(arg_0) << tmp_7)) : ((*(arg_0) & 0x3U) != 0x0 ?  + *(arg_0) * tmp_2 : __ROR__( + *(arg_0), tmp_5) ^ )) : tmp_15((((*(tmp_9) & 0x3U) != 0x1UL ? ((*(tmp_9) & 0x3U) == 0x2UL ? *(tmp_9) + tmp_10 + tmp_11 ^  :  - (*(arg_0) << tmp_7)) : ((*(tmp_9) & 0x3U) != 0x0 ?  + *(tmp_9) * tmp_2 : __ROR__( + *(tmp_9), tmp_5) ^ ))) ^ tmp_4, (((*(tmp_9) & 0x3U) != 0x1UL ? ((*(tmp_9) & 0x3U) == 0x2UL ? *(tmp_9) + tmp_10 + tmp_11 ^  :  - (*(tmp_9) << tmp_7)) : ((*(tmp_9) & 0x3U) != 0x0 ?  + *(tmp_9) * tmp_2 : __ROR__( + *(tmp_9), tmp_5) ^ ))) ^ tmp_4, 0x1bUL));
    tmp_17 =  + 0x1UL;
    tmp_18 = tmp_17;
    if (arg_1 != tmp_17) {
        return arg_0;
    }
    tmp_6 = 0x7feb352dU;
    if (arg_1 != tmp_18) {
        return arg_0;
    }
    return arg_0;
    return arg_0;
}

unsigned long _control_flow_showcase(unsigned long arg_0, unsigned long arg_1) {
    int exit_flag_0, tmp_10, tmp_11, tmp_12, tmp_13, tmp_15, tmp_16, tmp_17, tmp_18, tmp_19, tmp_20, tmp_21, tmp_9;
    unsigned int tmp_0, tmp_1, tmp_14, tmp_2, tmp_3, tmp_4, tmp_5, tmp_6, tmp_7, tmp_8;
    unsigned long ret_0;

    tmp_0 = 0x0U;
    tmp_1 = 0x0U;
    tmp_2 = 0x0U;
    arg_0 = 0x0U;
    tmp_3 = (arg_1 ^ tmp_4) & 0x3U;
    tmp_5 = (arg_1 == tmp_4 ? -(tmp_4) : arg_1);
    tmp_6 = arg_1 + tmp_4;
    tmp_7 = 0x2U;
    tmp_8 = 0x1U;
    tmp_9 = tmp_0;
    tmp_10 = tmp_3;
    tmp_11 = tmp_1;
    tmp_12 = tmp_2;
    tmp_13 = arg_0;
    while (true) {
        tmp_14 = tmp_9;
        tmp_0 = tmp_10;
        tmp_1 = tmp_11;
        tmp_2 = tmp_12;
        arg_0 = tmp_13;
        if ( == 0x2UL) {
            tmp_16 = (((arg_1 & tmp_4) == 0x0 ? tmp_2 : tmp_1)) + arg_0;
            arg_0 = tmp_16;
            tmp_0 = (((tmp_16 + 0x14UL == 0x0 ? 0x0UL : tmp_2 - 0x8UL)) != 0x0 ? tmp_7 + 0x1 : tmp_7);
            tmp_10 = tmp_0;
            tmp_13 = arg_0;
        } else {
            if (tmp_0 == 0x1UL) {
                exit_flag_0 = 0x2;
                break;
            }
            if (tmp_0 != 0x0) {
                exit_flag_0 = 0x2;
                break;
            }
            tmp_16 = tmp_4 + arg_0 + tmp_1;
            arg_0 = tmp_16;
            if ((tmp_16 & 0x1) != 0x0) {
                exit_flag_0 = 0x2;
                break;
            }
            tmp_10 = tmp_17;
            tmp_13 = arg_0;
            tmp_16 = arg_0 + tmp_5;
            arg_0 = tmp_16;
            if (tmp_16 > 0x32UL) {
                exit_flag_0 = 0x2;
                break;
            }
            exit_flag_0 = 0x1;
            break;
            tmp_0 = arg_0 >> 0x1fUL & 0x3U;
            tmp_10 = tmp_0;
            tmp_13 = arg_0;
            tmp_18 = tmp_6 + tmp_2;
            tmp_16 = tmp_18;
            if (tmp_18 == arg_0) {
                exit_flag_0 = 0x2;
                break;
            }
            exit_flag_0 = 0x0;
            break;
            tmp_16 ^= arg_0;
            arg_0 = tmp_16;
            tmp_0 = ((tmp_16 & 0x80000000U) == 0x0 ? tmp_7 & ~(tmp_14) : tmp_8);
            tmp_10 = tmp_0;
            tmp_13 = arg_0;
            tmp_10 = tmp_19;
            tmp_13 = arg_0;
        }
        tmp_15 = tmp_10;
        tmp_20 = tmp_13;
        tmp_16 =  - 0xc9UL;
        arg_1 = tmp_16;
        if (tmp_16 + 0x191UL >= 0x0) {
            if (tmp_2 + 0x1UL != 0xcUL) {
                exit_flag_0 = 0x2;
                break;
            }
            tmp_10 = tmp_15;
            tmp_13 = tmp_20;
            tmp_15 = tmp_10;
            tmp_21 = tmp_13;
            arg_0 = tmp_2 + 0x1;
            tmp_1--;
            tmp_0 = tmp_14 + 0x2;
            tmp_9 = tmp_0;
            tmp_10 = tmp_15;
            tmp_11 = tmp_1;
            tmp_12 = arg_0;
            tmp_13 = tmp_21;
            continue;
        }
        exit_flag_0 = 0x2;
        break;
    }
    if (exit_flag_0 == 0x0) {
        return ret_0;
    }
    if (exit_flag_0 == 0x1) {
        return ret_0;
    }
    if (( & 0x80000000) == 0x0) {
        tmp_4 = 0xfffffff7U;
    }
    return ret_0;
}

