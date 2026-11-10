#ifndef FASTMATH_H
#define FASTMATH_H

static unsigned long fast_mul16(unsigned int a, unsigned int b)
{
#ifdef __WATCOMC__
    unsigned int hi, lo;
    _asm {
        mov ax, a
        mul b
        mov lo, ax
        mov hi, dx
    }
    return ((unsigned long)hi << 16) | lo;
#else
    return (unsigned long)a * (unsigned long)b;
#endif
}

static long fast_mul16_signed(int a, unsigned int b)
{
    if (a < 0) return -(long)fast_mul16((unsigned int)(-a), b);
    return (long)fast_mul16((unsigned int)a, b);
}

static const unsigned int recip_table[128] = {
0,0,32768,21845,16384,13107,10922,9362,8192,7281,6553,5957,5461,5041,4681,4369,
4096,3855,3640,3449,3276,3120,2978,2849,2730,2621,2520,2427,2340,2259,2184,2114,
2048,1985,1927,1872,1820,1771,1725,1680,1638,1598,1560,1524,1489,1456,1425,1394,
1365,1337,1310,1285,1260,1236,1213,1191,1170,1150,1130,1111,1092,1074,1057,1040,
1024,1008,992,978,963,949,936,923,910,897,885,873,862,851,840,829,
819,809,799,789,780,770,761,752,744,735,727,719,711,703,696,689,
681,674,668,661,654,648,642,636,630,624,618,612,606,601,595,590,
585,580,574,569,564,559,555,550,546,541,537,532,528,524,520,516
};

static unsigned int fast_div_small(unsigned int num, unsigned int den)
{
    if (den == 1) return num;
    if (den < 128) {
        unsigned int r = recip_table[den];
#ifdef __WATCOMC__
        unsigned int result;
        _asm {
            mov ax, num
            mul r
            mov result, dx
        }
        return result;
#else
        return (unsigned int)(((unsigned long)num * (unsigned long)r) >> 16);
#endif
    }
    return num / den;
}

static unsigned int fast_div32_16(unsigned long dividend, unsigned int divisor,
                                   unsigned int *remainder)
{
    if ((dividend >> 16) >= divisor) {
        unsigned int quot = (unsigned int)(dividend / divisor);
        *remainder = (unsigned int)(dividend % divisor);
        return quot;
    }
#ifdef __WATCOMC__
    {
    unsigned int hi = (unsigned int)(dividend >> 16);
    unsigned int lo = (unsigned int)(dividend & 0xFFFFUL);
    unsigned int quot, rem;
    _asm {
        mov dx, hi
        mov ax, lo
        div divisor
        mov quot, ax
        mov rem, dx
    }
    *remainder = rem;
    return quot;
    }
#else
    {
    unsigned int quot = (unsigned int)(dividend / divisor);
    *remainder = (unsigned int)(dividend % divisor);
    return quot;
    }
#endif
}

static unsigned int fast_div16(unsigned long dividend, unsigned int divisor)
{
    unsigned int discard;
    return fast_div32_16(dividend, divisor, &discard);
}

#endif
