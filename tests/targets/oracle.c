void _analyze_series() {
    int exit_flag_0, tmp_1, tmp_10, tmp_17, tmp_3, tmp_5, tmp_7, tmp_8, tmp_9;
    unsigned int counter, tmp_11, tmp_12, tmp_13, tmp_14, tmp_15, tmp_16, tmp_18, tmp_2, tmp_6;
    unsigned long tmp_0, tmp_19;

    if (tmp_0 == 0x0) {
        tmp_1 = tmp_2 - 0x1;
        if (tmp_2 < 0x1UL) {
            while (true) {
                tmp_3 = tmp_1;
                counter = tmp_5;
                tmp_6 = tmp_7;
                tmp_2 = tmp_8;
                if (*(tmp_9) == 0x0) {
                    tmp_10 = tmp_11 & 0x3U;
                    tmp_1 = tmp_10 - 0x1;
                    if (tmp_10 > 0x1UL) {
                        if (tmp_10 != 0x0) {
                            counter += *(tmp_9) * tmp_2;
                            tmp_5 = counter;
                            tmp_7 = tmp_6;
                        } else {
                            counter += *(tmp_9);
                            tmp_5 = counter;
                            tmp_7 = tmp_6;
                        }
                        tmp_1 = tmp_10 - 0x2;
                        if (tmp_10 == 0x2UL) {
                            tmp_1 = *(tmp_9) - tmp_16;
                            counter += (*(tmp_9) != tmp_16 ? -(*(tmp_9)) : *(tmp_9));
                            tmp_5 = counter;
                            tmp_7 = tmp_6;
                        } else if ((*(tmp_9) & 0x1) != 0x0) {
                            if (*(tmp_9) + tmp_12 + counter <= tmp_13) {
                                exit_flag_0 = 0x0;
                                break;
                            }
                            tmp_14 = *(tmp_9) + tmp_12 + counter;
                            tmp_1 = *(tmp_9) + tmp_12 + counter - tmp_13;
                            tmp_5 = tmp_14;
                            tmp_7 = tmp_6;
                        } else {
                            counter -= tmp_15;
                            tmp_5 = counter;
                            tmp_7 = tmp_6;
                        }
                    }
                    tmp_1 = tmp_3;
                    tmp_5 = counter;
                    tmp_7 = tmp_6;
                }
                tmp_17 = tmp_1;
                tmp_3 = tmp_5;
                tmp_8 = tmp_7;
                tmp_2++;
                if (tmp_17 == 0x0) {
                    tmp_1 = tmp_17;
                    tmp_5 = tmp_3;
                    tmp_7 = tmp_8;
                    tmp_8 = tmp_2;
                    continue;
                }
                exit_flag_0 = 0x1;
                break;
            }
            if (exit_flag_0 == 0x0) {
                tmp_5 = tmp_14;
                tmp_7 = tmp_6;
            } else {
                tmp_5 = tmp_3;
                tmp_7 = tmp_8;
            }
            counter = tmp_5;
            tmp_8 = tmp_7;
            tmp_10 = (tmp_16 != 0x0 ? tmp_16 : tmp_18);
            counter += tmp_10;
            tmp_5 = counter;
            while (counter > 0x3e8UL) {
                counter = tmp_5;
                counter -= tmp_10;
                tmp_5 = counter;
            }
            tmp_5 = counter;
            tmp_7 = tmp_8;
            do {
                counter = tmp_5;
                tmp_6 = tmp_7;
                counter += tmp_6;
                tmp_6 >>= 0x1UL;
            } while (((tmp_6 == 0x1UL ? 0x0UL : counter & 0x1U)) != 0x0);
            break;
            tmp_5 = counter;
            tmp_7 = tmp_6;
            continue;
            return tmp_19;
        }
        return tmp_19;
    }
}

void _fold_bytes() {
    int tmp_2;
    unsigned long counter, tmp_0, tmp_3;

    if (tmp_0 == 0x0) {
        if (counter == 0x0) {
            while (counter != tmp_0) {
                tmp_0 = tmp_2;
                tmp_0++;
                tmp_2 = tmp_0;
            }
            if (counter != tmp_0) {
                return tmp_3;
            }
            return tmp_3;
        }
    }
    return tmp_3;
}

void _control_flow_showcase() {
    int exit_flag_0, tmp_0, tmp_10, tmp_12, tmp_13, tmp_14, tmp_17, tmp_4, tmp_5, tmp_6, tmp_7;
    unsigned int tmp_1, tmp_11, tmp_15, tmp_16, tmp_2, tmp_3, tmp_8, tmp_9;
    unsigned long tmp_18;

    tmp_0 = tmp_1 - 0xc9;
    tmp_2 = (tmp_0 ^ tmp_3) & 0x3U;
    tmp_4 = tmp_5;
    tmp_5 = tmp_2;
    tmp_6 = tmp_7;
    tmp_8 = tmp_4;
    tmp_2 = tmp_5;
    tmp_9 = tmp_10;
    tmp_11 = tmp_12;
    tmp_1 = tmp_6;
    switch (tmp_2) {
        case 1:
            if (tmp_1 > 0x32UL) {
                exit_flag_0 = 0x1;
            }
            if (tmp_2 != 0x0) {
                tmp_1 = tmp_3 + tmp_1 + tmp_9;
                if ((tmp_1 & 0x1) != 0x0) {
                    if (tmp_13 == tmp_1) {
                        exit_flag_0 = 0x0;
                        break;
                    }
                    tmp_5 = tmp_14;
                    tmp_6 = tmp_1;
                    tmp_1 += (tmp_0 == tmp_3 ? -(tmp_3) : tmp_0);
                    tmp_2 = tmp_1 >> 0x1fUL & 0x3U;
                    tmp_5 = tmp_2;
                    tmp_6 = tmp_1;
                    tmp_13 = tmp_0 + tmp_3 + tmp_11;
                    tmp_1 ^= tmp_13;
                    tmp_2 = ((tmp_1 & 0x80000000U) == 0x0 ? tmp_15 & ~(tmp_8) : tmp_16);
                    tmp_5 = tmp_2;
                    tmp_6 = tmp_1;
                    tmp_5 = tmp_17;
                    tmp_6 = tmp_1;
                }
            }
            break;
        case 2:
            tmp_1 += ((tmp_0 & tmp_3) == 0x0 ? tmp_11 : tmp_9);
            tmp_2 = (((tmp_1 + 0x14UL == 0x0 ? 0x0UL : tmp_11 - 0x8UL)) != 0x0 ? tmp_15 + 0x1 : tmp_15);
            tmp_5 = tmp_2;
            tmp_6 = tmp_1;
            tmp_12 = tmp_5;
            tmp_1 = tmp_6;
            tmp_5 = tmp_12;
            tmp_6 = tmp_1;
            tmp_12 = tmp_5;
            tmp_7 = tmp_6;
            tmp_11++;
            tmp_9--;
            tmp_8 += 0x2;
            if (tmp_11 != 0xcUL) {
                tmp_4 = tmp_8;
                tmp_5 = tmp_12;
                tmp_10 = tmp_9;
                tmp_12 = tmp_11;
                tmp_6 = tmp_7;
                continue;
            }
            break;
    }
    exit_flag_0 = 0x1;
    if (exit_flag_0 != 0x0) {
        return tmp_18;
    }
    return tmp_18;
}

