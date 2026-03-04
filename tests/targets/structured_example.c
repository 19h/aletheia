#include <stddef.h>
#include <stdint.h>

static int clamp_int(int value, int low, int high) {
    if (value < low) {
        return low;
    }
    if (value > high) {
        return high;
    }
    return value;
}

static uint32_t rotl32(uint32_t value, unsigned int amount) {
    amount &= 31U;
    if (amount == 0U) {
        return value;
    }
    return (value << amount) | (value >> (32U - amount));
}

int analyze_series(const int *values, int count, int threshold, int mode) {
    if (values == NULL || count <= 0) {
        return -1;
    }

    int accum = 0;
    int negatives = 0;
    int stop = 0;

    for (int i = 0; i < count; ++i) {
        int value = values[i];

        if (value == 0) {
            continue;
        }
        if (value < 0) {
            ++negatives;
            accum -= value;
            continue;
        }

        switch (mode & 3) {
            case 0:
                accum += value;
                break;
            case 1:
                accum += value * (i + 1);
                break;
            case 2:
                accum += (value > threshold) ? value : -value;
                break;
            default:
                if ((value & 1) != 0) {
                    accum += value * 3;
                    if (accum > 6000) {
                        stop = 1;
                    }
                } else {
                    accum -= value / 2;
                }
                break;
        }

        if (stop) {
            break;
        }
    }

    int step = threshold > 0 ? threshold : 7;
    while (accum > 1000) {
        accum -= step;
    }

    do {
        accum += negatives;
        negatives >>= 1;
    } while (negatives > 0 && (accum & 1) == 0);

    return clamp_int(accum, -2048, 2048);
}

uint32_t fold_bytes(const uint8_t *buffer, size_t length, uint32_t seed) {
    if (buffer == NULL || length == 0U) {
        return seed ^ 0x9e3779b9U;
    }

    uint32_t hash = seed ^ (uint32_t) length;
    for (size_t i = 0; i < length; ++i) {
        uint32_t byte = (uint32_t) buffer[i];

        switch (byte & 3U) {
            case 0:
                hash += byte * 0x45d9f3bU;
                break;
            case 1:
                hash ^= rotl32(byte + hash, (unsigned int) (i & 7U) + 3U);
                break;
            case 2:
                hash -= byte << (i & 5U);
                break;
            default:
                hash ^= (hash >> 3) + (byte * 33U);
                break;
        }

        if ((i & 15U) == 15U) {
            hash = rotl32(hash ^ 0xa5a5a5a5U, 5U);
        }
    }

    size_t rounds = (length & 7U) + 2U;
    while (rounds-- > 0U) {
        hash ^= hash >> 16;
        hash *= 0x7feb352dU;
        hash ^= hash >> 15;
    }

    return hash;
}

int control_flow_showcase(int x, int y) {
    int state = (x ^ y) & 3;
    int score = 0;

    for (int step = 0; step < 12; ++step) {
        switch (state) {
            case 0:
                score += x - step;
                if ((score & 1) == 0) {
                    state = 1;
                    continue;
                }
                state = 2;
                break;

            case 1:
                if (y > x) {
                    score += y;
                } else {
                    score -= x;
                }
                if (score > 50) {
                    return score;
                }
                state = (score < 0) ? 3 : 0;
                break;

            case 2:
                score += ((x & y) != 0) ? step : -step;
                if (step > 8 && score < -20) {
                    break;
                }
                state = 3;
                break;

            default:
                score ^= x + y + step;
                if (score == 0) {
                    return step;
                }
                if (score < 0) {
                    state = 1;
                } else {
                    state = (step & 1) != 0 ? 0 : 2;
                }
                break;
        }

        if (score > 200 || score < -200) {
            break;
        }
    }

    if (score < 0) {
        do {
            score += 9;
        } while (score < 0);
    }

    return score + state;
}